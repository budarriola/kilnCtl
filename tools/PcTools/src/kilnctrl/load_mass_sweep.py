"""Kiln-LOAD (thermal mass) sweep -- owner request 2026-09-02: every
identification, gain and coupling matrix in ``plant_sim.py`` was measured at
ONE load on the bench rig (whatever mass happened to be on it during the
excitation/cooldown runs) and the real bench kiln has never been fired at a
different one. This module asks whether current production gains and the
ADOPTED (``K_full``/``new``) coupling matrix survive a different load,
using ONLY the existing calibrated model -- no new simulator.

Kept as a separate, additive module rather than editing ``plant_sim.py``'s
core (another agent is concurrently sweeping fuzzy strength in that file):
everything below either calls existing ``plant_sim``/``plant_sim_sweep``
functions unchanged, or duplicates their per-tick loop locally with a
LOAD-SCALED plant standing in for the stock one.

THE LOAD MODEL -- read this before trusting a number below.

MEASURED (unchanged from plant_sim.py): K_full (steady-state coupling and
DC gain), tau (cooldown time constant), L (dead time), all fitted at
whatever load was on the rig for the three single-zone excitation runs and
three cooldowns (PID_EXPANSION_PLAN.md sec 3.2/3.4). That load is HELD as
the "empty" (1x) reference point below -- it is not literally an empty
kiln, it is "the load state the identification ran at," which is the only
anchor available.

ASSUMED, this module's own model of what changes with load:

  1. THERMAL MASS scales ``tau`` directly. ``tau = C_thermal / G_loss``
     (see plant_sim.py's ``RIG_C_THERMAL`` docstring); more ware/shelving
     adds sensible heat capacity (C_thermal) without changing the loss
     conductance (G_loss, a property of the walls/insulation, not the
     load) or the steady-state DC gain (K = P_max/G_loss, also
     load-independent by that same reasoning -- a bigger thermal mass
     takes longer to reach the same steady state, it does not change WHAT
     that steady state is). So ``tau_loaded = tau * mass_mult`` and
     ``K_full``'s DIAGONAL (own-zone DC gain) is left unchanged.

  2. CROSS-ZONE COUPLING is NOT assumed fixed. The owner's own framing is
     right: a loaded kiln changes radiative view factors between zones
     (shelves/ware between adjacent zones partially block the direct
     radiant path that produces the measured coupling) and can change
     convective coupling too (shelves interrupt the chamber's internal air
     circulation). This module treats coupling as a SECOND, independent
     axis: ``coupling_mult`` scales ONLY the OFF-DIAGONAL terms of
     ``K_full`` (``K_loaded = diag(K_diag) + coupling_mult * (K_full -
     diag(K_diag))``), leaving each zone's own DC gain and tau-scaling
     alone. 1.0 = bench-identified (unblocked) coupling; <1.0 models
     shelves/ware damping the inter-zone path, matching the same
     "shelves reduce coupling" reasoning ``PHYS_COUPLING_SEPARATION_DAMPING``
     already uses in plant_sim.py for the physical (cone-10) model, applied
     here as a swept variable instead of a fixed constant.

  Both axes are ASSUMED numbers (no multi-load hardware data exists to fit
  them from) but the STRUCTURE -- mass affects only the time constant,
  coupling-fraction affects only the off-diagonal gain -- follows directly
  from the K/tau/G_loss algebra that IS measured, not from a fresh guess.

  3. THE CONTROLLER NEVER SEES EITHER AXIS. ``coupled_ff_hold_climb`` is
     called with the plant's default (bench-identified) ``K_inv``/``tau``
     in every run below -- exactly like the real firmware, which has no
     load sensor and computes feedforward off one fixed, load-independent
     matrix regardless of what is actually in the kiln. This is the whole
     point of the sweep: does that fixed-matrix assumption survive a load
     change it cannot observe.

  Above ``plant_sim.EXTRAPOLATION_BOUNDARY_C`` (80 C) the plant is
  ``PhysicalKilnPlant``, which already carries its own explicit thermal-mass
  term (``PHYS_THERMAL_MASS_J_PER_K`` = wall mass + a flat "+10000 J/K,
  ASSUMED" shelves/ware allowance) and its own coupling-fraction term
  (``PHYS_COUPLING_FRAC``). ``LoadedPhysicalKilnPlant`` below scales those
  the same two ways -- mass_mult on thermal mass, coupling_mult on
  ``PHYS_COUPLING_FRAC`` -- so the cone-schedule run uses the identical
  load model as the profile-7 (low-temperature) run, just applied to the
  high-temperature plant's own parameterization instead of FOPDT's.

See ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` for where this is
recorded, and ``tests/test_load_mass_sweep.py`` for the mutation-tested
check that a mass increase actually degrades tracking in this model (i.e.
that this module's scaling isn't a silent no-op).
"""
from __future__ import annotations

import dataclasses
from typing import Optional, Sequence

import numpy as np

from . import plant_sim as ps

# ---------------------------------------------------------------------------
# Sweep axes
# ---------------------------------------------------------------------------

# mass_mult=1.0 is the load state the bench identification ran at (NOT
# necessarily a literally empty kiln -- see module docstring). Range chosen
# to span "however it was identified" up through "shelves stacked with ware
# on every level, a genuinely full kiln" -- 4x thermal mass is already a
# generous multiple of a modest ceramics load relative to the bare rig; a
# real kiln owner rarely loads much past that without also derating specific
# gravity per shelf, so 4x is treated as "realistically full" per the task.
MASS_MULTIPLIERS = {
    "1.0x_identified": 1.0,
    "1.5x_light_load": 1.5,
    "2.0x_moderate_load": 2.0,
    "3.0x_heavy_load": 3.0,
    "4.0x_full_load": 4.0,
}

# coupling_mult=1.0 reproduces the bench-identified (whatever-was-on-the-rig)
# off-diagonal coupling unchanged. <1.0 models shelves/ware progressively
# blocking the direct inter-zone radiant/convective path. Swept as its own
# axis rather than assumed fixed, per the owner's framing that load changes
# view factors as well as mass.
COUPLING_MULTIPLIERS = {
    "1.0x_unblocked": 1.0,
    "0.7x_shelved": 0.7,
    "0.4x_heavily_shelved": 0.4,
}


def loaded_K_tau(mass_mult: float, coupling_mult: float):
    """``K_loaded`` (diagonal unchanged, off-diagonal scaled by
    ``coupling_mult``) and ``tau_loaded`` (scaled by ``mass_mult``) -- see
    module docstring sec 1/2."""
    K_diag_mat = np.diag(ps.K_diag)
    K_loaded = K_diag_mat + coupling_mult * (ps.K_full - K_diag_mat)
    tau_loaded = ps.tau * mass_mult
    return K_loaded, tau_loaded


class LoadedPhysicalKilnPlant(ps.PhysicalKilnPlant):
    """``PhysicalKilnPlant`` with thermal mass and cross-zone coupling
    scaled per module docstring sec 3, for the >80 C (cone-schedule) run.

    REVIEW 2026-09-02 (Opus round 4, finding 6): this used to hand-copy
    ``PhysicalKilnPlant.step`` verbatim (own power, radiative loss,
    temperature clamp) with no tripwire against the two drifting -- and
    ``plant_sim.py`` is under active edit. ``PhysicalKilnPlant.step`` now
    reads its power/coupling/thermal-mass constants off instance
    attributes (``self._p_max_w``/``self._coupling_frac``/
    ``self._thermal_mass``) set in ``__init__`` instead of bare module
    globals, specifically so this subclass can override just those three
    values here and DELEGATE to the inherited ``step()`` unchanged --
    there is no longer a second copy of the per-tick physics to drift.
    ``tests/test_load_mass_sweep.py`` pins that ``mass_mult=1.0,
    coupling_mult=1.0`` reproduces ``PhysicalKilnPlant`` byte-for-byte."""

    def __init__(self, dt, ambient=20.0, start_temp=None, mass_mult=1.0, coupling_mult=1.0):
        super().__init__(dt, ambient=ambient, start_temp=start_temp)
        self.mass_mult = mass_mult
        self.coupling_mult = coupling_mult
        self._thermal_mass = ps.PHYS_THERMAL_MASS_J_PER_K * mass_mult
        self._coupling_frac = ps.PHYS_COUPLING_FRAC * coupling_mult
        # self._p_max_w left at the base class's value (own-zone element
        # power is not scaled by load -- see module docstring sec 1).


def _run_loop(plant, segs, kp, ki, kd, ambient, integral_floor='ff_hold'):
    """The same per-tick coupled-ff/PID loop as ``plant_sim.run_profile``,
    duplicated (not imported) so a load-scaled plant object can be driven
    without editing that function's signature. Controller feedforward
    always uses the module's fixed, bench-identified ``K_full``/``tau``
    (module docstring sec 3) regardless of what the plant itself was
    loaded with."""
    pids = [ps.PID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0) for _ in range(ps.N_ZONES)]
    total_t = segs[-1][1]
    times, targets, temps_log, duty_log = [], [], [], []
    duty = np.zeros(ps.N_ZONES)
    t = 0.0
    while t <= total_t:
        target_c = target_rate = None
        for si, (t0, t1, c0, c1, rate) in enumerate(segs):
            if t0 <= t <= t1 or si == len(segs) - 1:
                if rate == 0.0:
                    target_c, target_rate = c1, 0.0
                else:
                    target_c, target_rate = c0 + rate * (t - t0), rate
                break
        for i in range(ps.N_ZONES):
            hold, climb, ff = ps.coupled_ff_hold_climb(target_c, target_rate, i, ambient=ambient)
            duty[i], _ = pids[i].update(target_c, plant.temp[i], ps.DT, ff, hold,
                                         integral_floor=integral_floor)
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        plant.step(duty)
        t += ps.DT

    targets_arr = np.array(targets)
    return dict(t=np.array(times), target=targets_arr,
                temps=np.array(temps_log), duty=np.array(duty_log),
                seg_bounds=[s[1] for s in segs])


def run_profile7_loaded(mass_mult: float, coupling_mult: float,
                         kp=0.06, ki=0.0003, kd=0.0, ambient=20.0,
                         capture_path: Optional[str] = None) -> dict:
    """Profile 7 (ramp-dwell-ramp-dwell to 45/60 C, current production
    gains/ADOPTED matrix), driven off the same segment timing a real
    profile-7 capture ran, against a load-scaled ``FOPDTPlant``.
    ``capture_path`` defaults to the shipped ``final.jsonl`` fixture (the
    end-of-calibration, current-production build's own profile-7 run) so
    segment boundaries are real hardware timing, not invented ones."""
    from . import log_analysis as la
    if capture_path is None:
        import os
        capture_path = os.path.join(
            os.path.dirname(__file__), "..", "..", "tests", "fixtures", "plant_sim", "final.jsonl")
    rows_all = la.parse_profile_exec_jsonl(capture_path)
    runs = la.split_runs(rows_all)
    rows = runs[0]
    segs, start_c = ps.segs_from_capture(rows)
    start_temp = [start_c, start_c, start_c]

    K_loaded, tau_loaded = loaded_K_tau(mass_mult, coupling_mult)
    plant = ps.FOPDTPlant(K_loaded, tau_loaded, ps.L, ps.DT, ambient=ambient, start_temp=start_temp)
    result = _run_loop(plant, segs, kp, ki, kd, ambient)
    result.update(mass_mult=mass_mult, coupling_mult=coupling_mult, band="profile7", segs=segs)
    return result


def run_cone_schedule_loaded(mass_mult: float, coupling_mult: float,
                              kp=0.06, ki=0.0003, kd=0.0, ambient=20.0) -> dict:
    """C6DHSC ("Plainsman Cone 6 Drop-and-hold, Slow Cool",
    firmware/KilnFW/App/drivers/persist/profiles_builtin_table.inc), a real 5-segment
    built-in cone-6 schedule reaching 1204 C, against a load-scaled
    ``PhysicalKilnPlant`` (the >80 C model -- see ``plant_sim_sweep.py``'s
    own regime split). Ramp rates converted from the firmware's C/hr to the
    sim's C/s; dwells expanded to their real minutes, not truncated."""
    # (target_c, ramp_c_per_hr, dwell_min) -- transcribed verbatim from
    # profiles_builtin_table.inc's C6DHSC entry.
    schedule = [
        (121.0, 60.0, 60),
        (1148.0, 194.0, 15),
        (1204.0, 60.0, 10),
        (1148.0, 500.0, 30),
        (760.0, 83.0, 0),
    ]
    segs = []
    t = 0.0
    c_running = ambient
    for target_c, ramp_c_per_hr, dwell_min in schedule:
        # rate = sign(delta) * ramp_c_per_hr/3600, duration = |delta|/rate
        delta = target_c - c_running
        rate = math_copysign(ramp_c_per_hr / 3600.0, delta) if delta != 0 else 0.0
        ramp_dur = abs(delta) / (ramp_c_per_hr / 3600.0) if delta != 0 and ramp_c_per_hr > 0 else 0.0
        if ramp_dur > 0:
            segs.append((t, t + ramp_dur, c_running, target_c, rate))
            t += ramp_dur
        if dwell_min > 0:
            segs.append((t, t + dwell_min * 60.0, target_c, target_c, 0.0))
            t += dwell_min * 60.0
        c_running = target_c
    if not segs:
        segs = [(0.0, 1.0, ambient, ambient, 0.0)]

    start_temp = [ambient, ambient, ambient]
    plant = LoadedPhysicalKilnPlant(ps.DT, ambient=ambient, start_temp=start_temp,
                                     mass_mult=mass_mult, coupling_mult=coupling_mult)
    result = _run_loop(plant, segs, kp, ki, kd, ambient)
    result.update(mass_mult=mass_mult, coupling_mult=coupling_mult, band="cone6_c6dhsc", segs=segs)
    return result


import math as _math


def math_copysign(mag, sign_from):
    return _math.copysign(mag, sign_from)


# ---------------------------------------------------------------------------
# Metrics, in pid_ab_compare.py's metric set (mean/worst ramp error, dwell
# steady-state offset, dwell-entry overshoot peak/lag, duty saturation) so
# numbers here are directly comparable to the hardware table.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class LoadRunMetrics:
    band: str
    mass_mult: float
    coupling_mult: float
    zone: int
    segment_index: int
    phase: str
    mean_error_c: float
    worst_error_c: float          # max(|overshoot|, |undershoot|) in this window
    dwell_overshoot_peak_c: float  # peak error in the 220s after a ramp->dwell transition (dwell segs only)
    duty_saturated_frac: float    # fraction of this window's samples at duty>=0.999 on this zone


def compute_metrics(result: dict) -> list:
    t, target, temps, duty = result['t'], result['target'], result['temps'], result['duty']
    segs = result['segs']
    boundaries = [0.0] + result['seg_bounds']
    out = []
    for idx, (t0, t1) in enumerate(zip(boundaries[:-1], boundaries[1:])):
        rate = segs[idx][4]
        phase = "dwell" if rate == 0.0 else "ramp"
        m = (t >= t0) & (t <= t1)
        if not m.any():
            continue
        for z in range(ps.N_ZONES):
            err = temps[m, z] - target[m]
            overshoot_peak = 0.0
            if phase == "dwell":
                window2 = (t >= t0) & (t <= min(t1, t0 + 220.0))
                if window2.any():
                    overshoot_peak = float((temps[window2, z] - target[window2]).max())
            out.append(LoadRunMetrics(
                band=result['band'], mass_mult=result['mass_mult'], coupling_mult=result['coupling_mult'],
                zone=z, segment_index=idx, phase=phase,
                mean_error_c=float(err.mean()),
                worst_error_c=float(np.abs(err).max()),
                dwell_overshoot_peak_c=overshoot_peak,
                duty_saturated_frac=float((duty[m, z] >= 0.999).mean()),
            ))
    return out


def format_metrics_table(rows: Sequence[LoadRunMetrics]) -> str:
    header = (f"{'band':14s} {'mass':>6s} {'cpl':>5s} {'z':>1s} {'seg':>3s} {'phase':5s} "
              f"{'mean_err_c':>10s} {'worst_err_c':>11s} {'ovs_peak_c':>10s} {'sat_frac':>8s}")
    lines = [header]
    for r in rows:
        lines.append(
            f"{r.band:14s} {r.mass_mult:6.1f} {r.coupling_mult:5.2f} {r.zone:1d} {r.segment_index:3d} "
            f"{r.phase:5s} {r.mean_error_c:10.2f} {r.worst_error_c:11.2f} "
            f"{r.dwell_overshoot_peak_c:10.2f} {r.duty_saturated_frac:8.2f}"
        )
    return "\n".join(lines)
