#!/usr/bin/env python3
"""One-command CT calibration runner -- `firmware/SimFW/docs/DESIGN_NOTES.md`
section 3.3's calibration procedure (milestone M-D), and
`firmware/SaftyFW/docs/CURRENT_SENSE.md` section 5's commissioning check,
run together in a single bench-day pass:

  1. **Crosstalk check** (CURRENT_SENSE.md Sec.5 step 2, applied to the CT
     channels rather than a relay): drive each of the 3 CT channels alone,
     confirm exactly one DUT current-sense channel responds and it's the
     right one. Gates everything after it -- a calibration built on top of
     an unproven channel mapping is worse than none.
  2. **Sweep + fit** (DESIGN_NOTES.md 3.3 exit criterion): for each channel, sweep
     the commanded amplitude across N points, read back what `SaftyFW`'s
     current-sense channel reports, least-squares fit gain/offset, reject
     the fit if it isn't credible (see `fit.py`'s `evaluate_fit`).
  3. **Persist**: write a versioned JSON calibration table the PC keeps --
     see `calibration_table.py`'s module docstring for why this is a file,
     not fixture flash.

See this directory's README.md for the full procedure, what a good run
looks like, and what to do about a bad one. See its "Be honest about
limits" section before trusting a run against real hardware for the first
time.

Usage (bench day, real fixture + real DUT over the existing kilnctrl link):
    python calibrate_ct.py --port COM5 --dut-serial-port COM7 \\
        --out calibration_table.json

Usage (today, no hardware -- exercise the procedure against virtual_simfw
with a synthetic stand-in DUT):
    python calibrate_ct.py --virtual --mock-dut --out /tmp/cal.json
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
from calibration_table import (  # noqa: E402
    CalibrationTable,
    CalibrationTableError,
    ChannelCalibration,
    SweepPoint,
)
from crosstalk import CrosstalkResult, all_channels_clean, check_crosstalk  # noqa: E402
from fit import DEFAULT_MIN_R2, FitError, evaluate_fit, fit_linear  # noqa: E402

_PCTOOLS_SRC = Path(__file__).resolve().parents[4] / "tools" / "PcTools" / "src"
if str(_PCTOOLS_SRC) not in sys.path:
    sys.path.insert(0, str(_PCTOOLS_SRC))

from kilnsim.link import MockSimLink, SerialSimLink, SimLink, SimLinkError, TcpSimLink  # noqa: E402

import fixture  # noqa: E402
from readback import DutReadback, KilnctrlSafetyReadback, SyntheticDutReadback  # noqa: E402

DEFAULT_POINTS = 10  # DESIGN_NOTES.md 3.3: "sweep commanded amplitude across ~10 points"
DEFAULT_AMPS_MIN = 0.0
DEFAULT_AMPS_MAX = 1.0  # today, this IS the raw 0..1 PWM-scale range (identity placeholder)
DEFAULT_SETTLE_S = 0.2
DEFAULT_SAMPLES_PER_POINT = 3
DEFAULT_CROSSTALK_THRESHOLD_A = 2.0  # matches CURRENT_SENSE.md Sec.5 i_present_a "typical"
DEFAULT_CROSSTALK_TEST_AMPS = 1.0
DEFAULT_OUT = Path(__file__).resolve().parent / "ct_calibration_table.json"


def _make_fixture_link(args) -> SimLink:
    if args.mock:
        return MockSimLink()
    if args.virtual is not False:
        return TcpSimLink()
    return SerialSimLink()


def _connect_fixture(link: SimLink, args) -> str:
    if isinstance(link, TcpSimLink):
        address = args.virtual if isinstance(args.virtual, str) else None
        return link.connect(address)
    return link.connect(args.port)


def _make_dut_readback(args) -> DutReadback:
    if args.mock_dut:
        # Deliberately optimistic-but-not-trivial synthetic model: distinct
        # gain/offset per channel (so a bug that mixes up channel indices
        # would be caught by a wrong-looking fit), small noise, and channel
        # 2 leaking a bit into channel 1 UNLESS --mock-dut-clean is given --
        # the default demonstrates the crosstalk gate actually catching
        # something, per this task's "demonstrate the bad-data rejection
        # actually triggers" requirement.
        from readback import SyntheticChannelModel

        models = {
            0: SyntheticChannelModel(gain=28.0, offset=0.15, noise_sigma=0.05),
            1: SyntheticChannelModel(gain=31.5, offset=0.20, noise_sigma=0.05),
            2: SyntheticChannelModel(gain=26.2, offset=0.10, noise_sigma=0.05),
        }
        if args.mock_dut_leak:
            models[2].leak_into = {1: args.mock_dut_leak}
        if args.mock_dut_flat_channel is not None:
            models[args.mock_dut_flat_channel] = SyntheticChannelModel(
                gain=0.0, offset=0.05, noise_sigma=0.01
            )
        return SyntheticDutReadback(
            channel_models=models, commanded=args._commanded_state, seed=args.mock_dut_seed
        )

    # Real path: the existing kilnctrl SAFETY link, exactly what DESIGN_NOTES.md 3.3
    # calls "the existing kilnctrl MCP path" -- see readback.py's module
    # docstring for the full chain and its honesty caveats.
    sys.path.insert(0, str(_PCTOOLS_SRC))
    from kilnctrl.link_hub import get_shared_link  # noqa: E402
    from kilnctrl.safety import SafetyClient  # noqa: E402

    link = get_shared_link()
    if args.dut_serial_port:
        link.connect(args.dut_serial_port)
    return KilnctrlSafetyReadback(SafetyClient(link))


def _set_amps(link: SimLink, commanded_state: "Optional[dict[int, float]]", channel: int, amps: float) -> None:
    """Commands the fixture and, when a synthetic DUT is in use, records
    the exact value just sent so `SyntheticDutReadback` can read it back
    with zero extra round trips and zero race against the fixture's own
    async apply -- see `readback.py`'s `SyntheticDutReadback` docstring."""
    fixture.set_amps(link, channel, amps)
    if commanded_state is not None:
        commanded_state[channel] = amps


def _read_avg(dut: DutReadback, samples: int, settle_s: float) -> "tuple[float, float, float]":
    readings = []
    for i in range(samples):
        readings.append(dut.read_currents())
        if i < samples - 1:
            time.sleep(settle_s)
    n = len(readings)
    return tuple(sum(r[c] for r in readings) / n for c in range(3))  # type: ignore[return-value]


def run_crosstalk_check(
    link: SimLink,
    dut: DutReadback,
    channels: Sequence[int],
    test_amps: float,
    threshold_a: float,
    settle_s: float,
    samples: int,
    log,
    commanded_state: "Optional[dict[int, float]]" = None,
) -> "list[CrosstalkResult]":
    log("=== Step 1: crosstalk / channel-mapping check ===")
    log(f"driving each channel to {test_amps:g} alone, expecting exactly one "
        f"DUT channel to read above {threshold_a:g} A")

    for c in channels:
        fixture.set_mode_manual(link, c)
        _set_amps(link, commanded_state, c, 0.0)
    time.sleep(settle_s)

    results: "list[CrosstalkResult]" = []
    for c in channels:
        _set_amps(link, commanded_state, c, test_amps)
        time.sleep(settle_s)
        currents = _read_avg(dut, samples, settle_s)
        result = check_crosstalk(c, currents, threshold_a)
        log(f"  channel {c}: readback={[f'{a:.3f}' for a in currents]} A -> "
            f"{'PASS' if result.passed else 'FAIL'}: {result.message}")
        results.append(result)
        _set_amps(link, commanded_state, c, 0.0)
        time.sleep(settle_s)

    return results


def run_sweep(
    link: SimLink,
    dut: DutReadback,
    channel: int,
    amps_min: float,
    amps_max: float,
    points: int,
    settle_s: float,
    samples: int,
    log,
    commanded_state: "Optional[dict[int, float]]" = None,
) -> "list[SweepPoint]":
    log(f"-- sweeping channel {channel}: {points} points from {amps_min:g} to {amps_max:g} --")
    fixture.set_mode_manual(link, channel)
    step = (amps_max - amps_min) / (points - 1) if points > 1 else 0.0
    out: "list[SweepPoint]" = []
    for i in range(points):
        commanded = amps_min + step * i
        _set_amps(link, commanded_state, channel, commanded)
        time.sleep(settle_s)
        currents = _read_avg(dut, samples, settle_s)
        measured = currents[channel]
        log(f"  commanded={commanded:.4f} -> measured ch{channel}={measured:.4f} A "
            f"(others: {[f'{a:.3f}' for i2, a in enumerate(currents) if i2 != channel]})")
        out.append(SweepPoint(commanded=commanded, measured_a=measured))
    _set_amps(link, commanded_state, channel, 0.0)
    return out


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "One-command CT calibration: DESIGN_NOTES.md 3.3 (M-D) sweep+fit, plus the "
            "CURRENT_SENSE.md Sec.5 one-channel-responds crosstalk gate."
        )
    )
    fx = p.add_argument_group("fixture (SimFW) connection")
    fx.add_argument("--port", default=None, help="serial port for a real SimFW fixture")
    fx.add_argument("--virtual", nargs="?", const=True, default=False,
                     help="connect to virtual_simfw over TCP instead of real hardware "
                          "(optional host:port, default 127.0.0.1:<ephemeral default>)")
    fx.add_argument("--mock", action="store_true", help="use kilnsim's in-memory MockSimLink")

    dut = p.add_argument_group("DUT (SaftyFW) readback")
    dut.add_argument("--dut-serial-port", default=None,
                      help="serial port for the real kilnctrl link to the ESP/Pico pair "
                           "(omit to use kilnctrl's autodetect)")
    dut.add_argument("--mock-dut", action="store_true",
                      help="use a synthetic stand-in DUT instead of real kilnctrl hardware "
                           "-- see readback.py's module docstring for exactly what this "
                           "does and does not prove")
    dut.add_argument("--mock-dut-seed", type=int, default=0)
    dut.add_argument("--mock-dut-leak", type=float, default=0.15,
                      help="fraction of channel 2's synthetic signal leaked into channel 1's "
                           "reading, to demonstrate the crosstalk gate catching something "
                           "(default 0.15; pass 0 for a clean synthetic DUT)")
    dut.add_argument("--mock-dut-flat-channel", type=int, default=None, choices=[0, 1, 2],
                      help="make one synthetic channel read flat (gain=0), to demonstrate "
                           "the bad-fit rejection")

    sweep = p.add_argument_group("sweep parameters")
    sweep.add_argument("--channels", default="0,1,2", help="comma-separated channel list")
    sweep.add_argument("--points", type=int, default=DEFAULT_POINTS)
    sweep.add_argument("--amps-min", type=float, default=DEFAULT_AMPS_MIN)
    sweep.add_argument("--amps-max", type=float, default=DEFAULT_AMPS_MAX)
    sweep.add_argument("--settle-s", type=float, default=DEFAULT_SETTLE_S)
    sweep.add_argument("--samples-per-point", type=int, default=DEFAULT_SAMPLES_PER_POINT)
    sweep.add_argument("--min-r2", type=float, default=DEFAULT_MIN_R2)

    ct = p.add_argument_group("crosstalk check parameters")
    ct.add_argument("--crosstalk-threshold-a", type=float, default=DEFAULT_CROSSTALK_THRESHOLD_A)
    ct.add_argument("--crosstalk-test-amps", type=float, default=DEFAULT_CROSSTALK_TEST_AMPS)
    ct.add_argument("--skip-crosstalk", action="store_true",
                     help="DANGEROUS: skip the crosstalk gate and fit anyway. Only for "
                          "re-running a sweep you already know is clean; never use this on "
                          "first bring-up of new wiring.")

    p.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"calibration table output path (default {DEFAULT_OUT})")
    p.add_argument("--report-out", type=Path, default=None,
                    help="optional: also write the full run report (including a failed "
                         "run's data) as JSON to this path")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    channels = [int(c) for c in args.channels.split(",") if c.strip() != ""]

    def log(line: str = "") -> None:
        print(line)

    log("=== SimFW CT calibration (DESIGN_NOTES.md 3.3 / CURRENT_SENSE.md Sec.5) ===")
    log(f"channels={channels} points={args.points} amps=[{args.amps_min},{args.amps_max}] "
        f"min_r2={args.min_r2} crosstalk_threshold_a={args.crosstalk_threshold_a}")

    link = _make_fixture_link(args)
    try:
        used = _connect_fixture(link, args)
    except SimLinkError as exc:
        log(f"ERROR: could not connect to fixture: {exc}")
        return 3
    log(f"fixture connected: {used}")

    # Shared {channel: last-commanded-amps} state -- populated by every
    # `_set_amps()` call below. Only consumed by `SyntheticDutReadback`
    # (see `_make_dut_readback`); harmless, unused overhead on the real
    # kilnctrl-backed path.
    commanded_state: "dict[int, float]" = {c: 0.0 for c in channels}
    args._commanded_state = commanded_state
    try:
        dut = _make_dut_readback(args)
    except Exception as exc:  # noqa: BLE001
        log(f"ERROR: could not connect to DUT readback: {exc}")
        link.disconnect()
        return 3
    log(f"DUT readback: {'synthetic (--mock-dut)' if args.mock_dut else 'kilnctrl SAFETY link (real)'}")

    report: dict = {"channels": {}, "crosstalk": [], "ok": False}
    exit_code = 0
    try:
        if args.skip_crosstalk:
            log("=== Step 1: crosstalk check SKIPPED (--skip-crosstalk) ===")
            crosstalk_results: "list[CrosstalkResult]" = []
            crosstalk_ok = True
        else:
            crosstalk_results = run_crosstalk_check(
                link, dut, channels,
                test_amps=args.crosstalk_test_amps,
                threshold_a=args.crosstalk_threshold_a,
                settle_s=args.settle_s,
                samples=args.samples_per_point,
                log=log,
                commanded_state=commanded_state,
            )
            crosstalk_ok = all_channels_clean(crosstalk_results)
        report["crosstalk"] = [
            {"channel": r.driven_channel, "responded": list(r.responded), "passed": r.passed, "message": r.message}
            for r in crosstalk_results
        ]

        if not crosstalk_ok:
            log("")
            log("CROSSTALK CHECK FAILED -- refusing to sweep/fit or write a calibration "
                "table. Fix the wiring/mapping and re-run. (CURRENT_SENSE.md Sec.5: 'a "
                "correlation guard fed by a mis-mapped CT is worse than no guard'.)")
            return 1

        log("")
        log("=== Step 2: sweep + fit ===")
        channel_cals: "dict[int, ChannelCalibration]" = {}
        any_fit_failed = False
        for c in channels:
            sweep_points = run_sweep(
                link, dut, c,
                amps_min=args.amps_min, amps_max=args.amps_max, points=args.points,
                settle_s=args.settle_s, samples=args.samples_per_point, log=log,
                commanded_state=commanded_state,
            )
            report["channels"][c] = {"sweep": [{"commanded": p.commanded, "measured_a": p.measured_a} for p in sweep_points]}
            try:
                fit = fit_linear(
                    [p.commanded for p in sweep_points],
                    [p.measured_a for p in sweep_points],
                )
            except FitError as exc:
                log(f"  channel {c}: FIT ERROR: {exc}")
                report["channels"][c]["fit_error"] = str(exc)
                any_fit_failed = True
                continue

            verdict = evaluate_fit(fit, min_r2=args.min_r2)
            log(f"  channel {c}: gain={fit.gain:.4f} offset={fit.offset:.4f} r2={fit.r2:.4f} "
                f"max_abs_residual={fit.max_abs_residual:.4f} -> {verdict.describe()}")
            report["channels"][c].update({
                "gain": fit.gain, "offset": fit.offset, "r2": fit.r2,
                "max_abs_residual_a": fit.max_abs_residual, "verdict": verdict.describe(),
            })
            if not verdict.ok:
                any_fit_failed = True
                continue
            channel_cals[c] = ChannelCalibration.from_fit(c, fit, sweep_points)

        log("")
        if any_fit_failed:
            log("ONE OR MORE CHANNEL FITS REJECTED -- refusing to write a calibration "
                "table (a partial table would silently leave the rejected channel(s) on "
                "whatever they had before, which is worse than an obvious failure). Check "
                "the log above for the specific reason(s), fix them, and re-run.")
            exit_code = 2
        else:
            table = CalibrationTable.new(crosstalk_passed=crosstalk_ok, channels=channel_cals)
            try:
                table.save(args.out)
            except CalibrationTableError as exc:
                log(f"ERROR saving calibration table: {exc}")
                return 4
            log(f"Calibration table written: {args.out}")
            log("")
            log("=== Calibration table ===")
            log(json.dumps(table.to_dict(), indent=2, sort_keys=True))
            report["ok"] = True

        return exit_code
    finally:
        if args.report_out:
            args.report_out.parent.mkdir(parents=True, exist_ok=True)
            args.report_out.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
        try:
            dut.close()
        except Exception:  # noqa: BLE001 - best-effort cleanup
            pass
        link.disconnect()


if __name__ == "__main__":
    raise SystemExit(main())
