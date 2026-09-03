"""In-flight kiln-LOAD (thermal mass) estimator -- offline analysis tool,
owner priority 2026-09-02: PID_EXPANSION_PLAN.md sec 3.8 found tracking
degrading with mass but concluded load is "not observable from any capture
in this repo today," and named the next step: an in-flight duty-vs-rate fit
compared against the identified tau. This module is that fit. It changes
nothing about firmware behaviour -- read-only analysis plus offline
validation against ``load_mass_sweep.py``'s simulated loaded plants and the
real profile-7 captures in ``logs/coupling/``.

THE ESTIMATOR

``load_mass_sweep.py``'s own model (adopted here unchanged, see its
docstring sec 1) says thermal mass scales ONLY the per-zone time constant
``tau``; DC gain ``K`` and dead time ``L`` are load-independent. The plant
obeys (``plant_sim.FOPDTPlant.step``, low-temperature FOPDT regime only --
see LIMITATIONS below):

    dT_i/dt = (u_ss_i - T_i) / tau_i
    u_ss_i  = ambient + K_i . duty_delayed

where ``duty_delayed`` is the commanded duty vector delayed by the zone's
own dead time ``L_i``. Given ``K`` (the ADOPTED coupling matrix, already on
the board), ``ambient``, and ``L`` (already identified), a capture of
``duty`` and ``T_i`` over time lets us solve for ``tau_i`` directly:

    dT_i/dt = (1/tau_i) * (u_ss_i - T_i)

This is a single-parameter linear regression through the origin: plot
``dT_i/dt`` (measured, from consecutive temperature samples) against the
"drive" term ``u_ss_i - T_i`` (computed from delayed duty and the KNOWN
matrix) and the best-fit slope is ``1/tau_i``. ``mass_mult_i = tau_i_est /
tau_i_identified``.

ASSUMPTIONS (inherited from load_mass_sweep.py, restated for this module):

  1. Mass scales tau only; K (steady-state gain/coupling) and L (dead time)
     are load-independent. If a real load also perturbs K or L -- plausible,
     never checked against hardware -- this estimator's ``u_ss`` computation
     is biased and the resulting mass estimate absorbs that bias too.
  2. K, tau (the 1.0x reference), L, ambient are all already known constants
     (identified, on the board / in this repo) -- the estimator needs no new
     firmware logging, only what a profile_exec poll capture already carries:
     commanded duty and actual_c per zone, at the existing poll cadence.
  3. dT/dt is estimated by simple consecutive-sample finite differencing on
     whatever cadence the capture used (~1 s for the sim, ~1 s for the real
     pollers here) -- no filtering beyond the OLS fit itself. A capture with
     a coarser or jittery cadence will be noisier; this module does not
     attempt to resample.
  4. Regression window quality: samples where the drive term
     ``|u_ss - T|`` is small (near steady state) are excluded by
     ``min_drive_c`` -- near a dwell the denominator of the per-sample
     tau estimate blows up and pure sensor noise dominates. Ramp segments,
     where the drive term stays large, are the well-conditioned regime;
     this is stated as a finding in VALIDATION below, not assumed.

LIMITATIONS

  - Low-temperature (<=80 C, FOPDT) regime only -- matches every capture
    this module was validated against. The >80 C PhysicalKilnPlant model
    was not addressed (out of scope: no real high-temperature loaded
    capture exists to check it against, per sec 3.8/3.7's open gap).
  - A single global "mass_mult" per zone, not per-firing-segment; the
    estimator is not told whether load changed mid-firing (it did not, in
    any capture used here).
  - Validated in simulation using the SAME K/L this module's u_ss uses to
    generate the sim data in the first place (load_mass_sweep.py holds
    coupling_mult=1.0, i.e. the ADOPTED matrix unperturbed) -- see
    ``docs`` sec 3.8 update for how that inflates apparent accuracy
    relative to hardware, where K itself carries the sim's own held-out
    RMS error (z0 1.05 / z1 0.61 / z2 0.67 C, PID_EXPANSION_PLAN.md sec 3.2).

See ``tools/PcTools/tests/test_load_estimator.py`` for the mutation-tested
checks, and ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.8 for the
recorded verdict.
"""
from __future__ import annotations

import dataclasses
from typing import Optional, Sequence

import numpy as np

from . import plant_sim as ps
from . import log_analysis as la
from . import http_capture_log as hcl


@dataclasses.dataclass
class ZoneLoadEstimate:
    zone: int
    tau_est_s: float
    mass_mult_est: float
    n_samples_used: int
    r2: float          # goodness of the through-origin fit, 1.0 = perfect
    slope_se: float     # standard error of the fitted slope (1/tau), 0 if <3 samples


def _delay_duty(duty: np.ndarray, dt: float, L_i: float) -> np.ndarray:
    """``duty[:, zone]`` shifted forward in time by ``L_i`` seconds (i.e.
    ``delayed[k] = duty[k - steps]``), zero-order-held before t=0 with the
    first sample -- same convention as ``FOPDTPlant``'s own duty_hist
    (constant zero-initialized history, but real captures start mid-firing
    so holding the first real sample is the closer assumption here)."""
    steps = int(round(L_i / dt))
    if steps <= 0:
        return duty.copy()
    delayed = np.empty_like(duty)
    delayed[:steps] = duty[0]
    delayed[steps:] = duty[:-steps] if steps < len(duty) else duty[0]
    return delayed


def estimate_zone_mass_mult(
    t: np.ndarray, temps: np.ndarray, duty: np.ndarray, zone: int,
    ambient: float = 20.0, K: Optional[np.ndarray] = None,
    tau_ref: Optional[np.ndarray] = None, L: Optional[np.ndarray] = None,
    min_drive_c: float = 5.0,
) -> Optional[ZoneLoadEstimate]:
    """Fits ``dT/dt = (1/tau) * (u_ss - T)`` for one zone over the whole
    supplied capture (caller slices to whatever window -- e.g. one ramp
    segment -- it wants estimated). Returns ``None`` if fewer than 3 usable
    samples survive the ``min_drive_c`` filter (see module docstring
    assumption 4).

    ``t`` must be uniformly spaced (as every capture used here is); ``dt``
    is taken from ``t[1] - t[0]``.
    """
    K = ps.K_full if K is None else K
    tau_ref = ps.tau if tau_ref is None else tau_ref
    L = ps.L if L is None else L

    dt = float(t[1] - t[0])
    # duty is (N, N_ZONES); delay each zone's own commanded duty by THIS
    # zone's dead time (matches FOPDTPlant.step, which delays the whole
    # duty vector by the RECEIVING zone's own L).
    duty_delayed = np.column_stack([
        _delay_duty(duty[:, j], dt, L[zone]) for j in range(duty.shape[1])
    ])
    u_ss = ambient + duty_delayed @ K[zone]
    T = temps[:, zone]
    drive = u_ss - T

    dTdt = np.gradient(T, dt)

    # The first L[zone] seconds of ANY window are unreliable: the delay
    # reconstruction has no duty history predating the window, so it
    # zero-order-holds the window's first sample backward. If the real
    # (unknown) pre-window duty differed -- the ordinary case, since a
    # window rarely starts exactly one dead-time after a duty change --
    # the reconstructed u_ss is wrong for up to L[zone] seconds while
    # T's actual response still reflects the true (unknown) history.
    # These points combine a large, wrong drive with a near-zero real
    # dT/dt and bias the through-origin slope down (tau up) when pooled
    # with well-conditioned points; measured directly in validation
    # (see test_load_estimator.py) -- without this exclusion a 4.0x sim
    # run reads back as ~4.5x. Excluded rather than trusted.
    warmup_steps = int(round(L[zone] / dt))
    n_total = len(t)
    valid_start = np.zeros(n_total, dtype=bool)
    valid_start[warmup_steps:] = True

    mask = (np.abs(drive) >= min_drive_c) & valid_start
    n = int(mask.sum())
    if n < 3:
        return None

    x = drive[mask]
    y = dTdt[mask]
    denom = float(np.dot(x, x))
    if denom <= 0:
        return None
    slope = float(np.dot(x, y) / denom)
    if slope <= 0:
        # Non-physical (would imply negative or infinite tau) -- refuse
        # rather than return a nonsense multiplier.
        return None

    resid = y - slope * x
    ss_res = float(np.dot(resid, resid))
    ss_tot = float(np.dot(y - y.mean(), y - y.mean()))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else float("nan")
    # SE of the through-origin OLS slope: sigma^2 / sum(x^2)
    sigma2 = ss_res / (n - 1) if n > 1 else float("nan")
    slope_se = float(np.sqrt(sigma2 / denom)) if n > 1 else 0.0

    tau_est = 1.0 / slope
    mass_mult = tau_est / tau_ref[zone]
    return ZoneLoadEstimate(zone=zone, tau_est_s=tau_est, mass_mult_est=mass_mult,
                             n_samples_used=n, r2=r2, slope_se=slope_se)


def estimate_all_zones(t, temps, duty, ambient=20.0, K=None, tau_ref=None, L=None,
                        min_drive_c=3.0) -> list:
    out = []
    for z in range(temps.shape[1]):
        est = estimate_zone_mass_mult(t, temps, duty, z, ambient=ambient, K=K,
                                       tau_ref=tau_ref, L=L, min_drive_c=min_drive_c)
        if est is not None:
            out.append(est)
    return out


# ---------------------------------------------------------------------------
# Real-capture loading: pull t/temps/duty arrays straight out of a
# profile_exec poll capture (logs/coupling/*_http.jsonl), the same rows
# plant_sim.segs_from_capture reads.
# ---------------------------------------------------------------------------

def arrays_from_capture(rows: Sequence["la.PollRow"]):
    """Returns ``(t, temps, duty)`` -- ``t`` seconds from the first row
    (using each row's own ``elapsed_s``, NOT assumed 1 Hz, though every
    capture used here happens to poll at 1 Hz), ``temps``/``duty`` shape
    ``(N, 3)`` ordered by zone number."""
    zones_present = sorted(rows[0].zones.keys())
    t = np.array([r.elapsed_s for r in rows], dtype=float)
    temps = np.array([[r.zones[z].actual_c for z in zones_present] for r in rows])
    duty = np.array([[r.zones[z].duty for z in zones_present] for r in rows])
    return t, temps, duty


def load_capture_rows(path: str) -> list:
    """Parses a real capture regardless of which of the two on-disk
    envelopes it uses: the bare ``HH:MM:SS {profile_exec body}`` poll-log
    form (``log_analysis.parse_profile_exec_jsonl`` -- what the
    ``tests/fixtures/plant_sim/*.jsonl`` rested-start captures use), or the
    HTTP-capture ``{"t":..., "exec": {...}, "status": ...}`` wrapper
    (``http_capture_log.poll_rows`` -- what ``logs/coupling/p7_*_http.jsonl``
    uses). Tries the bare form first and falls back to the wrapped form if
    that yields nothing, rather than requiring the caller to know which."""
    rows = la.parse_profile_exec_jsonl(path)
    if rows:
        return rows
    return hcl.poll_rows(path)


def estimate_from_capture_path(path: str, run_index: int = 0, ambient: float = 20.0,
                                min_drive_c: float = 5.0, min_elapsed_s: float = 0.0) -> list:
    """Convenience wrapper: parse a real capture (either on-disk envelope,
    see ``load_capture_rows``), split multi-run files, take ``run_index``,
    drop the first ``min_elapsed_s`` (dead-time/startup transient), and
    estimate every zone's mass multiplier off the rest."""
    rows_all = load_capture_rows(path)
    runs = la.split_runs(rows_all)
    rows = runs[run_index]
    t, temps, duty = arrays_from_capture(rows)
    keep = t >= (t[0] + min_elapsed_s)
    return estimate_all_zones(t[keep], temps[keep], duty[keep], ambient=ambient,
                               min_drive_c=min_drive_c)
