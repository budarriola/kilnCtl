"""Parallel high-temperature sweep over ``plant_sim``'s calibrated model.

SIMULATOR ONLY. This module never touches hardware -- it does not import
``serial_link``/``display``/anything that talks to a board, and nothing here
raises a zone's ``max_temp_c`` or starts/stops a firing on the real kiln.
It exists specifically so a firing up to cone 10 (~1285 C) can be explored
without ever running that temperature on the physical test-fixture kiln --
see ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.4.

Runs many simulated firings concurrently (one process per (band, matrix
variant) pair, via ``multiprocessing.Pool``) and collects them into one
table in the PARENT process, in a fixed deterministic order -- workers never
interleave output or race a shared file, so a sweep is exactly reproducible
given the same bands/matrices/gains.

Every result carries ``max_target_c``/``extrapolation`` (see
``plant_sim.EXTRAPOLATION_BOUNDARY_C``): bands at or below 80 C are fit to
measured data, bands above it (bisque/cone 6/cone 10) run on the ASSUMED
radiative extension in ``plant_sim.loss_conductance_scale`` and must be read
as extrapolation.

CLI:
    python -m kilnctrl.plant_sim_sweep run
    python -m kilnctrl.plant_sim_sweep run --bands 30,55,80,1000,1222,1285 \\
        --matrices old,new --workers 8 --json
"""
from __future__ import annotations

import argparse
import dataclasses
import multiprocessing
from typing import Optional, Sequence

import numpy as np

from . import plant_sim as ps

# Named temperature bands: low bands sit inside the measured 0-80 C rig
# envelope (sanity anchors -- a sweep entry here should look like the
# existing fixture comparisons); the high bands are real firing targets a
# kiln reaches and MUST run only in this simulator, never on hardware.
DEFAULT_BANDS = {
    "z0_low_30c": 30.0,
    "z0_low_55c": 55.0,
    "z0_low_80c": 80.0,
    "bisque_1000c": 1000.0,
    "cone6_1222c": 1222.0,
    "cone10_1285c": 1285.0,
}

RAMP_RATE_C_PER_S = 3.0 / 60.0  # 3 C/min -- inside profile 7's measured 2-3.5 C/min range
DWELL_S = 900.0
AMBIENT_C = 20.0


@dataclasses.dataclass
class SweepResult:
    band: str
    target_c: float
    matrix_variant: str
    extrapolation: bool
    hold_infeasible: bool
    duty_saturated_frac: float          # fraction of dwell samples at duty==1.0, any zone
    max_duty: float
    overshoot_peak_c: float             # max post-ramp overshoot across zones, in the 220s window
    overshoot_peak_lag_s: float         # time of that peak after ramp end
    dwell_end_mean_abs_error_c: float   # |sim - target| at the end of the dwell, mean over zones


def _band_segs(target_c: float):
    ramp_dur = max(1.0, (target_c - AMBIENT_C) / RAMP_RATE_C_PER_S)
    segs = [
        (0.0, ramp_dur, AMBIENT_C, target_c, RAMP_RATE_C_PER_S),
        (ramp_dur, ramp_dur + DWELL_S, target_c, target_c, 0.0),
    ]
    return segs


def _run_one(args) -> dict:
    band_name, target_c, matrix_name, kp, ki, kd = args
    K_variant = ps.MATRIX_VARIANTS[matrix_name]
    K_inv_variant = np.linalg.inv(K_variant)
    segs = _band_segs(target_c)
    start_temp = [AMBIENT_C, AMBIENT_C, AMBIENT_C]

    result = ps.run_profile(
        segs, start_temp, kp=kp, ki=ki, kd=kd,
        climb_mode='coupled', integral_floor='ff_hold', ambient=AMBIENT_C,
        controller_K_inv=K_inv_variant, controller_tau=ps.tau,
    )
    t, target, temps, duty = result['t'], result['target'], result['temps'], result['duty']
    ramp_end = segs[0][1]

    hold_infeasible = ps.hold_duty_infeasible(target_c, K_inv=K_inv_variant, ambient=AMBIENT_C)

    dwell_mask = t > ramp_end
    duty_saturated_frac = float((duty[dwell_mask] >= 0.999).any(axis=1).mean()) if dwell_mask.any() else 0.0
    max_duty = float(duty.max()) if len(duty) else 0.0

    window_mask = (t >= ramp_end) & (t <= ramp_end + 220.0)
    overshoot_peak_c = 0.0
    overshoot_peak_lag_s = 0.0
    if window_mask.any():
        err = temps[window_mask] - target[window_mask, None]
        flat_idx = np.argmax(err)
        row, _ = np.unravel_index(flat_idx, err.shape)
        overshoot_peak_c = float(err.max())
        overshoot_peak_lag_s = float(t[window_mask][row] - ramp_end)

    dwell_end_mask = t >= (t[-1] - 60.0) if len(t) else np.array([])
    if dwell_end_mask.any():
        dwell_end_mean_abs_error_c = float(np.abs(temps[dwell_end_mask] - target[dwell_end_mask, None]).mean())
    else:
        dwell_end_mean_abs_error_c = float("nan")

    return dataclasses.asdict(SweepResult(
        band=band_name, target_c=target_c, matrix_variant=matrix_name,
        extrapolation=ps.is_extrapolation(target_c),
        hold_infeasible=hold_infeasible,
        duty_saturated_frac=duty_saturated_frac,
        max_duty=max_duty,
        overshoot_peak_c=overshoot_peak_c,
        overshoot_peak_lag_s=overshoot_peak_lag_s,
        dwell_end_mean_abs_error_c=dwell_end_mean_abs_error_c,
    ))


def run_sweep(bands: dict[str, float], matrices: Sequence[str],
              kp: float = 0.06, ki: float = 0.0003, kd: float = 0.0,
              workers: int = 4) -> list[dict]:
    """Run every (band, matrix) combination, in parallel across ``workers``
    processes, and return results in a DETERMINISTIC order: bands sorted by
    target_c, then matrices in the order given -- independent of which
    worker finishes first, so two runs with the same inputs produce byte
    identical table order. Aggregation itself (building this list from
    worker results) happens only in the parent process.
    """
    tasks = []
    band_items = sorted(bands.items(), key=lambda kv: kv[1])
    for band_name, target_c in band_items:
        for matrix_name in matrices:
            tasks.append((band_name, target_c, matrix_name, kp, ki, kd))

    if workers <= 1:
        results = [_run_one(t) for t in tasks]
    else:
        with multiprocessing.Pool(processes=workers) as pool:
            unordered = pool.map(_run_one, tasks)
        # pool.map already preserves input order regardless of completion
        # order, but keyed re-sort makes that guarantee explicit rather than
        # relying on an implementation detail.
        keyed = {(r["band"], r["matrix_variant"]): r for r in unordered}
        results = [keyed[(band_name, matrix_name)]
                   for band_name, _ in band_items for matrix_name in matrices]
    return results


def format_sweep_table(results: Sequence[dict]) -> str:
    header = (f"{'band':16s} {'target_c':>9s} {'matrix':>6s} {'extrap':>6s} "
              f"{'infeas':>6s} {'sat_frac':>8s} {'max_duty':>8s} "
              f"{'ovs_c':>6s} {'ovs_lag_s':>9s} {'dwell_err_c':>11s}")
    lines = [header]
    for r in results:
        lines.append(
            f"{r['band']:16s} {r['target_c']:9.1f} {r['matrix_variant']:>6s} "
            f"{str(r['extrapolation']):>6s} {str(r['hold_infeasible']):>6s} "
            f"{r['duty_saturated_frac']:8.2f} {r['max_duty']:8.3f} "
            f"{r['overshoot_peak_c']:6.2f} {r['overshoot_peak_lag_s']:9.1f} "
            f"{r['dwell_end_mean_abs_error_c']:11.2f}"
        )
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-plant-sim-sweep",
        description="Parallel temperature-band sweep over the calibrated plant simulator (simulator only, never hardware).",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="run the sweep and print a table")
    p_run.add_argument("--bands", type=str, default=None,
                        help="comma list of target_c values (default: the named DEFAULT_BANDS set)")
    p_run.add_argument("--matrices", type=str, default="old,new",
                        help="comma list of matrix variants to test (old,new)")
    p_run.add_argument("--kp", type=float, default=0.06)
    p_run.add_argument("--ki", type=float, default=0.0003)
    p_run.add_argument("--kd", type=float, default=0.0)
    p_run.add_argument("--workers", type=int, default=min(8, multiprocessing.cpu_count()))
    p_run.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)
    if args.cmd != "run":
        parser.print_help()
        return 2

    if args.bands:
        vals = [float(x) for x in args.bands.split(",")]
        bands = {f"band_{v:.0f}c": v for v in vals}
    else:
        bands = DEFAULT_BANDS

    matrices = [m.strip() for m in args.matrices.split(",") if m.strip()]
    for m in matrices:
        if m not in ps.MATRIX_VARIANTS:
            parser.error(f"unknown matrix variant {m!r}, choices: {sorted(ps.MATRIX_VARIANTS)}")

    results = run_sweep(bands, matrices, kp=args.kp, ki=args.ki, kd=args.kd, workers=args.workers)

    if args.json:
        import json
        print(json.dumps(results, indent=2))
    else:
        print(format_sweep_table(results))
        print(f"\nEXTRAPOLATION_BOUNDARY_C={ps.EXTRAPOLATION_BOUNDARY_C} "
              f"(MEASURED data covers this range; bands above run on ASSUMED "
              f"radiative loss physics -- see plant_sim.RAD_LOSS_FRACTION_AT_REF)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
