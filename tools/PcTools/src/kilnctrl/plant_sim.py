"""Calibrated FOPDT+coupled-PID plant simulator for kilnCtl's 3-zone profile
executor -- promoted from a session scratchpad's ``calibrated_sim.py`` /
``calibration_compare.py`` pair once the calibration held up across five
hardware captures (baseline / after / ifix / holdfix_clean / final -- see
``tools/PcTools/tests/fixtures/plant_sim/`` and the "TRUST" section below).

THE BUG THIS REPLACES (read before touching ``run_profile`` or
``segs_from_capture``): every simulator upstream of this one
(``ease_off_sim.py``, ``dwell_entry_sim.py``, ``pid_floor_sim.py`` and this
module's own direct ancestor ``calibrated_sim.py`` before its first commit)
drove itself off a HARDCODED ramp rate, ``rate_c_per_s = 10.0/60.0`` (10
C/min). The real profile the five captures ran ramps at 2-3.5 C/min --
3-5x slower. Because the coupled climb feedforward is linear in the
commanded rate (``climb = tau * rate / K``), that single wrong constant
overstated and saturated the climb term on every ramp, handed the PID a
large wrong-sign correction to fight for the whole ramp, and produced a
phantom 5-7 C mean ramp error that gain retuning could not fix (it wasn't a
gain problem). It made the old simulator reject a change hardware later
liked, and endorse one hardware measured as worse.

The fix: ``run_profile()`` takes ramp rate, segment timing and start
temperature as EXPLICIT arguments -- there is no default and no fallback
constant anywhere in this module. ``segs_from_capture()`` /
``run_profile_from_capture()`` derive all three directly from a capture's
own logged segment boundaries (``elapsed_s``/``target_c`` at each
``segment_index``/``dwelling`` change), so the sim is always driven by
exactly the profile hardware ran. ``tests/test_plant_sim.py::
test_regression_reproduces_after_capture`` pins this: it fails hard if
``run_profile``/``segs_from_capture`` regress to a hardcoded rate.

TWO KNOBS reproduce the *known* control differences between the five
firmware builds (see ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 2
and 4), so one plant identification has to explain all five runs rather
than being re-fit per capture:

  climb_mode='uncoupled'   baseline's defect: each zone computes its climb
                            term as though it heated alone
                            (tau*rate/K_dc, no coupling matrix) --
                            documented as "a roughly tenfold over-drive on
                            zone 0."
  climb_mode='coupled'     'after' onward: the real coupled Gaussian solve
                            (K_full^-1), same math the firmware ships.
  integral_floor='ff_u'    ifix build: integral floors at -ff_u (the WHOLE
                            feedforward, climb included) -- the
                            rejected/superseded approach.
  integral_floor='ff_hold' holdfix/final: floors at -ff_hold (hold
                            component only) -- the shipped fix.

TRUST -- what this simulator can and cannot be used for. Read this before
using it to argue for or against a control change; it is not a validated
oracle.

  TRUST it for: ramp-tracking magnitude and sign on any build using the
  coupled feedforward (climb_mode='coupled', i.e. 'after' onward).
  Residuals against the five captures run 0.0-1.6 C on windows where
  hardware's own run-to-run RMS is 0.7-2.8 C -- comparable to hardware's
  own noise floor. Aggregate over all 60 windows (5 captures x 2 ramp + 2
  dwell segments x 3 zones): mean|residual| = 1.00 C, RMS = 1.45 C, max =
  4.15 C (that max is the still-open baseline/uncoupled case below).

  DO NOT TRUST it for the exact saturation dynamics of the pre-coupled
  (climb_mode='uncoupled') baseline defect: the sim's plain per-zone
  formula gets the *direction* and *rough scale* of the "tenfold
  over-drive" right (it overshoots seg0's ramp by 2.5-4.2 C, its largest
  residuals in the whole calibration) but not the real bug's precise
  saturation curve. Treat 'uncoupled' results as qualitative only.

  DO NOT TRUST zone 2's second-dwell number in isolation on any coupled
  build: it runs 1.8-2.6 C cold vs hardware on every one of 'after',
  'ifix', 'holdfix_clean' and 'final', specifically on seg1's dwell,
  zone 2, and nowhere else this consistently. This is NOT a simulator
  defect -- it is the sim faithfully reproducing a known, still-open
  plant-identification error: PID_EXPANSION_PLAN.md sec 3.2 documents
  zone 2's coupled solve wanting 0.861 hold duty where the kiln actually
  needs under 0.74, i.e. the identified K_dc for zone 2 is too low. Read a
  zone-2 dwell result from this sim as "probably 1-2 C optimistic or
  pessimistic depending on sign" until sec 3.2's plant re-identification
  lands and this module is recalibrated against a fresh capture.

  DO NOT TRUST absolute values below the ~1 C residual noise floor above.
  The identified plant (K/tau/L below) came from a single 0-80 C bench-rig
  dataset; that identification, not this module's mechanics, is the
  limiting factor on precision.

  Bottom line: good for mechanism-level "will this help or hurt, roughly
  how much" comparisons on coupled-ff builds. Not good enough to sign off
  a specific gain change to the third decimal place, or to replace a
  firing for zone 2's dwell behavior specifically.

Not modeled: PWM window quantization (``heater_output.c``) -- averages out
under 10 s sampling and is not implicated by any of the five captures'
error shape. Thermocouple resolution is not separately modeled either; the
five captures are already hardware-quantized, and this module's own output
is compared against them at their native 10 s cadence, never resampled or
smoothed to hide quantization on either side (see
``tests/fixtures/plant_sim/README.md`` and
``tests/test_plant_sim.py``'s module docstring for how the regression test
keeps that honest).

See ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` for the control-side
background this module reproduces, and ``log_analysis.py`` (whose
``parse_profile_exec_jsonl``/``split_runs``/``build_windows``/
``window_zone_stats`` this module reuses rather than re-implementing) for
the hardware-side windowing this simulator is compared against.
"""
from __future__ import annotations

import argparse
import dataclasses
from typing import Optional, Sequence

import numpy as np

from . import log_analysis

# ---------------------------------------------------------------------------
# Identified plant (0-80 C bench rig, single dataset -- see module docstring's
# TRUST section: this identification, not the simulator mechanics, is now the
# limiting factor on absolute precision).
# ---------------------------------------------------------------------------

K_diag = np.array([39.25, 31.97, 31.68])
tau = np.array([263.8, 269.8, 270.9])
L = np.array([52.8, 43.5, 33.9])

# wire_form[stepped][affected]; K_full[affected][stepped] is what the
# coupled solve needs (each column is "how much every zone rises per unit
# duty on the stepped zone").
_wire_form = np.array([
    [39.25, 15.78, 9.70],
    [26.61, 31.97, 11.38],
    [20.73, 21.09, 31.68],
])
K_full = _wire_form.T
_K_INV = np.linalg.inv(K_full)

DT = 1.0
N_ZONES = 3


class FOPDTPlant:
    """First-order-plus-dead-time plant, one instance per zone, coupled
    through ``K`` (each zone's steady state depends on every zone's duty)."""

    def __init__(self, K, tau, L, dt, ambient=20.0, start_temp=None):
        self.K, self.tau, self.L, self.dt, self.ambient = K, tau, L, dt, ambient
        self.n = len(tau)
        self.temp = np.full(self.n, ambient) if start_temp is None else np.array(start_temp, dtype=float)
        self.max_delay = int(np.max(L) / dt) + 2
        self.duty_hist = [np.zeros(self.n) for _ in range(self.max_delay)]

    def step(self, duty):
        duty = np.clip(duty, 0.0, 1.0)
        self.duty_hist.append(duty.copy())
        self.duty_hist.pop(0)
        new_temp = self.temp.copy()
        for i in range(self.n):
            delay_steps = min(int(round(self.L[i] / self.dt)), len(self.duty_hist) - 1)
            d_delayed = self.duty_hist[-1 - delay_steps]
            u_ss = self.ambient + np.dot(self.K[i], d_delayed)
            dTdt = (u_ss - self.temp[i]) / self.tau[i]
            new_temp[i] = self.temp[i] + dTdt * self.dt
        self.temp = new_temp
        return self.temp.copy()


class PID:
    """Line-for-line match to ``pid.c``'s ``pid_update_terms()``: conditional
    integration freeze, feedforward-relative floor, P/I/D/ff clamp
    structure. See sim_calibration.md sec 1 for the line-by-line check
    against the firmware source this was built against."""

    def __init__(self, kp, ki, kd, d_tau, b, pid_range_c):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.d_tau, self.b, self.pid_range_c = d_tau, b, pid_range_c
        self.integral = 0.0
        self.d_filtered = 0.0
        self.prev_measurement = None
        self.initialized = False

    def update(self, setpoint, measurement, dt_s, ff_u, ff_hold, integral_floor='ff_hold'):
        if not self.initialized:
            self.prev_measurement = measurement
            self.integral = 0.0
            self.d_filtered = 0.0
            self.initialized = True
        if dt_s <= 0:
            dt_s = 1.0
        error = setpoint - measurement
        if abs(error) > self.pid_range_c:
            self.prev_measurement = measurement
            u = 1.0 if error > 0 else 0.0
            return u, dict(p=u, i=0.0, d=0.0, ff=0.0)

        raw_d = -(measurement - self.prev_measurement) / dt_s
        alpha = dt_s / (self.d_tau + dt_s)
        self.d_filtered += alpha * (raw_d - self.d_filtered)
        self.prev_measurement = measurement

        p_term = self.kp * (self.b * setpoint - measurement)
        d_term = self.kd * self.d_filtered

        unclamped = p_term + self.ki * self.integral + d_term + ff_u
        would_push_further = (unclamped >= 1.0 and error > 0) or (unclamped <= 0.0 and error < 0)
        if not would_push_further:
            self.integral += error * dt_s

        i_term = self.ki * self.integral
        floor = -ff_u if integral_floor == 'ff_u' else -ff_hold
        if i_term < floor:
            i_term = floor
            self.integral = floor / self.ki if self.ki > 0 else 0.0
        elif i_term > 1.0:
            i_term = 1.0
            self.integral = 1.0 / self.ki if self.ki > 0 else 0.0

        u = p_term + i_term + d_term + ff_u
        u = min(max(u, 0.0), 1.0)
        return u, dict(p=p_term, i=i_term, d=d_term, ff=ff_u)


def coupled_ff_hold_climb(target_c, target_rate, i, ambient=20.0):
    """The shipped coupled solve ('after' onward): hold and climb duty for
    zone i, solved jointly across all three zones via K_full^-1."""
    rhs_hold = np.full(N_ZONES, target_c - ambient)
    hold_duty = _K_INV @ rhs_hold
    rhs_climb = tau * target_rate
    climb_duty = _K_INV @ rhs_climb
    hold_i = hold_duty[i]
    climb_i = climb_duty[i]
    total = hold_i + climb_i
    total_clamped = min(max(total, 0.0), 1.0)
    hold_out = min(max(hold_i, 0.0), 1.0)
    climb_out = total_clamped - hold_out
    return hold_out, climb_out, total_clamped


def uncoupled_ff_hold_climb(target_c, target_rate, i, ambient=20.0):
    """Baseline's defect formula: each zone as though it heated alone --
    (T_sp - T_amb)/K_dc + rate*tau/K_dc, no coupling matrix at all. Gets the
    baseline build's over-drive DIRECTION and rough SCALE right, not its
    precise saturation dynamics -- see module docstring's TRUST section."""
    hold_i = (target_c - ambient) / K_diag[i]
    climb_i = target_rate * tau[i] / K_diag[i]
    total = hold_i + climb_i
    total_clamped = min(max(total, 0.0), 1.0)
    hold_out = min(max(hold_i, 0.0), 1.0)
    climb_out = total_clamped - hold_out
    return hold_out, climb_out, total_clamped


def run_profile(segs, start_temp, kp=0.06, ki=0.0003, kd=0.0,
                 climb_mode='coupled', integral_floor='ff_hold', ambient=20.0):
    """Run the plant+PID loop over an explicit segment list.

    ``segs``: list of ``(t0, t1, c0, c1, rate)`` tuples, ``rate`` signed
    C/s, ``0.0`` for a dwell. There is deliberately NO default profile and
    NO hardcoded rate here -- see the module docstring's "THE BUG THIS
    REPLACES" section. Every caller must supply real segment timing, either
    by hand or via ``segs_from_capture``/``run_profile_from_capture``.
    """
    plant = FOPDTPlant(K_full, tau, L, DT, ambient=ambient, start_temp=start_temp)
    pids = [PID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0) for _ in range(N_ZONES)]
    ff_fn = coupled_ff_hold_climb if climb_mode == 'coupled' else uncoupled_ff_hold_climb

    total_t = segs[-1][1]
    times, targets, temps_log, duty_log = [], [], [], []
    duty = np.zeros(N_ZONES)
    t = 0.0
    while t <= total_t:
        seg_idx = None
        for si, (t0, t1, c0, c1, rate) in enumerate(segs):
            if t0 <= t <= t1 or si == len(segs) - 1:
                seg_idx = si
                if rate == 0.0:
                    target_c = c1
                    target_rate = 0.0
                else:
                    target_c = c0 + rate * (t - t0)
                    target_rate = rate
                break
        for i in range(N_ZONES):
            hold, climb, ff = ff_fn(target_c, target_rate, i, ambient=ambient)
            duty[i], _ = pids[i].update(target_c, plant.temp[i], DT, ff, hold, integral_floor=integral_floor)
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        plant.step(duty)
        t += DT

    return dict(t=np.array(times), target=np.array(targets),
                temps=np.array(temps_log), duty=np.array(duty_log),
                seg_bounds=[s[1] for s in segs])


def segs_from_capture(rows: Sequence[log_analysis.PollRow]):
    """Builds the ``(t0,t1,c0,c1,rate)`` segment list ``run_profile()``
    needs directly from a parsed capture's rows (a SINGLE run -- call
    ``log_analysis.split_runs`` first for a multi-run file). Segment
    boundaries are exactly where the real firmware's own
    ``segment_index``/``dwelling`` changed and the ramp rate is derived
    from the capture's own elapsed time and target temperature at each
    boundary -- NOT assumed. This is the fix for the bug described at the
    top of this module: the old sims hardcoded a rate here instead of
    reading it off the capture.
    """
    bounds = []
    prev_key = None
    for r in rows:
        key = (r.segment_index, r.dwelling)
        if key != prev_key:
            bounds.append((r.elapsed_s, r.target_c, r.dwelling))
            prev_key = key
    bounds.append((rows[-1].elapsed_s, rows[-1].target_c, rows[-1].dwelling))

    segs = []
    start_c = None
    for z in rows[0].zones.values():
        start_c = z.actual_c
        break
    c_running = start_c
    for k in range(len(bounds) - 1):
        t0, c_at_start, dwelling = bounds[k]
        t1, c_end, _ = bounds[k + 1]
        if t1 <= t0:
            continue
        if dwelling:
            segs.append((t0, t1, c_end, c_end, 0.0))
            c_running = c_end
        else:
            rate = (c_end - c_running) / (t1 - t0) if t1 > t0 else 0.0
            segs.append((t0, t1, c_running, c_end, rate))
            c_running = c_end
    return segs, start_c


def run_profile_from_capture(rows: Sequence[log_analysis.PollRow], **kwargs):
    """``run_profile`` driven entirely off a real capture's own segment
    boundaries -- see ``segs_from_capture``. Returns ``(result, segs)``."""
    segs, start_c = segs_from_capture(rows)
    start_temp = [start_c, start_c, start_c]
    return run_profile(segs, start_temp, **kwargs), segs


# ---------------------------------------------------------------------------
# Sim-side windowed stats, in the same shape as log_analysis.ZoneWindowStats,
# so a sim run and a hardware capture can be reported and diffed side by
# side.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class SimZoneWindowStats:
    zone: int
    n_samples: int
    mean_error_c: float
    rms_error_c: float
    max_overshoot_c: float
    max_undershoot_c: float


def sim_window_zone_stats(result: dict, t0: float, t1: float, zone: int) -> Optional[SimZoneWindowStats]:
    t, target, temps = result['t'], result['target'], result['temps']
    m = (t >= t0) & (t <= t1)
    if not m.any():
        return None
    err = temps[m, zone] - target[m]
    return SimZoneWindowStats(
        zone=zone,
        n_samples=int(m.sum()),
        mean_error_c=float(err.mean()),
        rms_error_c=float(np.sqrt((err ** 2).mean())),
        max_overshoot_c=float(max(err.max(), 0.0)),
        max_undershoot_c=float(max(-err.min(), 0.0)),
    )


def render_sim_report(segs, start_temp, kp=0.06, ki=0.0003, kd=0.0,
                       climb_mode='coupled', integral_floor='ff_hold', ambient=20.0) -> dict:
    """Run the plant model over ``segs`` and report per-window, per-zone
    stats in the shape ``log_analysis.render_firing_report`` reports for
    real hardware -- so sim output is directly comparable to a hardware
    firing report without a separate mental model. No hardware capture
    required (contrast ``render_sim_vs_capture_report``, which additionally
    diffs against one)."""
    result = run_profile(segs, start_temp, kp=kp, ki=ki, kd=kd,
                          climb_mode=climb_mode, integral_floor=integral_floor, ambient=ambient)
    windows = []
    t_arr = result['t']
    boundaries = [0.0] + result['seg_bounds']
    for idx, (t0, t1) in enumerate(zip(boundaries[:-1], boundaries[1:])):
        rate = segs[idx][4]
        phase = "dwell" if rate == 0.0 else "ramp"
        zstats = {}
        for z in range(N_ZONES):
            s = sim_window_zone_stats(result, t0, t1, z)
            if s:
                zstats[z] = s
        windows.append(dict(segment_index=idx, phase=phase, start_s=t0, end_s=t1, zones=zstats))
    return dict(kp=kp, ki=ki, kd=kd, climb_mode=climb_mode, integral_floor=integral_floor,
                windows=windows, result=result)


def format_sim_report_text(report: dict) -> str:
    lines = [
        f"plant_sim run: climb_mode={report['climb_mode']} integral_floor={report['integral_floor']} "
        f"kp={report['kp']} ki={report['ki']} kd={report['kd']}",
    ]
    for w in report["windows"]:
        lines.append(f"-- seg {w['segment_index']} {w['phase']:5s} [{w['start_s']:.0f}s..{w['end_s']:.0f}s]")
        for z in sorted(w["zones"]):
            s = w["zones"][z]
            lines.append(
                f"    z{z}: mean_err={s.mean_error_c:+.2f}C rms={s.rms_error_c:.2f}C "
                f"over={s.max_overshoot_c:.2f}C under={s.max_undershoot_c:.2f}C n={s.n_samples}"
            )
    return "\n".join(lines)


def render_sim_vs_capture_report(path: str, run_idx: int = 0, kp=0.06, ki=0.0003, kd=0.0,
                                  climb_mode='coupled', integral_floor='ff_hold', ambient=20.0) -> dict:
    """Simulate the exact profile a hardware capture ran (via
    ``segs_from_capture``) and diff the sim's windowed per-zone stats
    against ``log_analysis``'s own windowed stats for the same capture.
    This is the calibration comparison itself, promoted to a reusable
    function -- see ``tests/test_plant_sim.py`` for the pinned regression
    that uses this against ``tests/fixtures/plant_sim/after.jsonl``.
    """
    rows_all = log_analysis.parse_profile_exec_jsonl(path)
    runs = log_analysis.split_runs(rows_all)
    if run_idx >= len(runs):
        return {"error": f"{path}: run_idx {run_idx} out of range ({len(runs)} runs in file)"}
    rows = runs[run_idx]

    result, segs = run_profile_from_capture(rows, kp=kp, ki=ki, kd=kd,
                                             climb_mode=climb_mode, integral_floor=integral_floor, ambient=ambient)
    windows = log_analysis.build_windows(rows)
    zones = log_analysis.zones_in_rows(rows)

    rows_out = []
    residuals = []
    for w in windows:
        if w.duration_s <= 0:
            continue
        for z in zones:
            hw = log_analysis.window_zone_stats(w, rows, z)
            if hw is None:
                continue
            sw = sim_window_zone_stats(result, w.start_s, w.end_s, z)
            if sw is None:
                continue
            d_mean = sw.mean_error_c - hw.mean_error_c
            d_rms = sw.rms_error_c - hw.rms_error_c
            residuals.append(d_mean)
            rows_out.append(dict(
                segment_index=w.segment_index, phase=w.phase, zone=z,
                hw_mean=hw.mean_error_c, hw_rms=hw.rms_error_c,
                sim_mean=sw.mean_error_c, sim_rms=sw.rms_error_c,
                d_mean=d_mean, d_rms=d_rms,
            ))

    resid = np.array(residuals) if residuals else np.array([0.0])
    return dict(
        path=path, run_idx=run_idx, runs_in_file=len(runs),
        kp=kp, ki=ki, kd=kd, climb_mode=climb_mode, integral_floor=integral_floor,
        windows=rows_out,
        n_windows=len(rows_out),
        mean_abs_residual_c=float(np.abs(resid).mean()),
        rms_residual_c=float(np.sqrt((resid ** 2).mean())),
        max_abs_residual_c=float(np.abs(resid).max()),
    )


def format_sim_vs_capture_report_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [
        f"sim vs hardware: {report['path']} run #{report['run_idx'] + 1}/{report['runs_in_file']}  "
        f"climb_mode={report['climb_mode']} integral_floor={report['integral_floor']} "
        f"kp={report['kp']} ki={report['ki']} kd={report['kd']}",
    ]
    for w in report["windows"]:
        lines.append(
            f"  seg{w['segment_index']} {w['phase']:5s} z{w['zone']} "
            f"HW mean={w['hw_mean']:+.3f} rms={w['hw_rms']:.3f} | "
            f"SIM mean={w['sim_mean']:+.3f} rms={w['sim_rms']:.3f} | "
            f"d_mean={w['d_mean']:+.3f} d_rms={w['d_rms']:+.3f}"
        )
    lines.append(
        f"n_windows={report['n_windows']}  mean|resid|={report['mean_abs_residual_c']:.3f} C  "
        f"rms(resid)={report['rms_residual_c']:.3f} C  max|resid|={report['max_abs_residual_c']:.3f} C"
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-plant-sim",
        description=__doc__.splitlines()[0] if __doc__ else "kilnCtl calibrated plant simulator",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_cmp = sub.add_parser("compare", help="simulate a hardware capture's own profile and diff against it")
    p_cmp.add_argument("jsonl_path")
    p_cmp.add_argument("--run", type=int, default=0, help="which run in a multi-run file (default 0)")
    p_cmp.add_argument("--kp", type=float, default=0.06)
    p_cmp.add_argument("--ki", type=float, default=0.0003)
    p_cmp.add_argument("--kd", type=float, default=0.0)
    p_cmp.add_argument("--climb-mode", choices=["coupled", "uncoupled"], default="coupled")
    p_cmp.add_argument("--integral-floor", choices=["ff_hold", "ff_u"], default="ff_hold")
    p_cmp.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "compare":
        report = render_sim_vs_capture_report(
            args.jsonl_path, run_idx=args.run, kp=args.kp, ki=args.ki, kd=args.kd,
            climb_mode=args.climb_mode, integral_floor=args.integral_floor,
        )
        if args.json:
            import json
            print(json.dumps(report, indent=2))
        else:
            print(format_sim_vs_capture_report_text(report))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
