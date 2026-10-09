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
import itertools
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
    min_drive_c: float = 5.0, L_pair: Optional[np.ndarray] = None,
) -> Optional[ZoneLoadEstimate]:
    """Fits ``dT/dt = (1/tau) * (u_ss - T)`` for one zone over the whole
    supplied capture (caller slices to whatever window -- e.g. one ramp
    segment -- it wants estimated). Returns ``None`` if fewer than 3 usable
    samples survive the ``min_drive_c`` filter (see module docstring
    assumption 4).

    ``t`` must be uniformly spaced (as every capture used here is); ``dt``
    is taken from ``t[1] - t[0]``.

    ``L_pair`` (added 2026-09-03): the ORIGINAL version of this function
    delayed every column of ``duty`` by the same single ``L[zone]`` -- this
    zone's OWN dead time -- before weighting by ``K[zone]``. Sec 2/3.8's
    diagnosis is that this is exactly wrong for the off-diagonal (neighbour)
    columns, whose real dead time (135-158 s, ASSUMED single 146.5 s
    midpoint -- see ``plant_sim.L_PAIR``) runs 3-4x the diagonal's (34-53
    s): a neighbour's duty from ~100 s ago was being credited to *this*
    zone's drive term ~100 s too early. Passing ``L_pair`` delays column j
    of the drive term by ``L_pair[zone, j]`` instead of a single scalar.
    **Defaults to ``None``, which reproduces the ORIGINAL single-delay
    behaviour exactly** (``L[zone]`` broadcast to every column) so every
    existing caller -- including ``load_mass_sweep.py``'s simulated data,
    generated with the single-delay ``FOPDTPlant`` -- is untouched; this
    argument is purely additive. Pass ``plant_sim.L_PAIR`` explicitly to opt
    into the per-path reconstruction (see
    ``tests/test_load_estimator.py``'s per-path-vs-single-delay checks and
    PID_EXPANSION_PLAN.md sec 3.8's 2026-09-03 update for what changes when
    a caller does).
    """
    K = ps.K_full if K is None else K
    tau_ref = ps.tau if tau_ref is None else tau_ref
    L = ps.L if L is None else L
    L_pair = np.tile(np.asarray(L).reshape(-1, 1), (1, len(L))) if L_pair is None else L_pair

    dt = float(t[1] - t[0])
    # duty is (N, N_ZONES); delay column j by L_pair[zone, j] -- the dead
    # time on the PATH from stepped zone j to this (receiving) zone, not a
    # single per-zone scalar applied to every column (see docstring above).
    duty_delayed = np.column_stack([
        _delay_duty(duty[:, j], dt, L_pair[zone, j]) for j in range(duty.shape[1])
    ])
    u_ss = ambient + duty_delayed @ K[zone]
    T = temps[:, zone]
    drive = u_ss - T

    dTdt = np.gradient(T, dt)

    # The first max(L_pair[zone, :]) seconds of ANY window are unreliable:
    # the delay reconstruction has no duty history predating the window, so
    # it zero-order-holds the window's first sample backward for whichever
    # column's delay is longest (now the OFF-DIAGONAL columns, ~146.5 s,
    # not the ~34-53 s diagonal the original single-delay version excluded
    # for). If the real (unknown) pre-window duty differed -- the ordinary
    # case -- the reconstructed u_ss is wrong for up to that long while T's
    # actual response still reflects the true (unknown) history. These
    # points combine a large, wrong drive with a near-zero real dT/dt and
    # bias the through-origin slope down (tau up) when pooled with
    # well-conditioned points; measured directly in validation (see
    # test_load_estimator.py) -- without this exclusion a 4.0x sim run
    # reads back as ~4.5x. Excluded rather than trusted.
    warmup_steps = int(round(np.max(L_pair[zone, :]) / dt))
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


@dataclasses.dataclass
class PerSourceLoadEstimate:
    """Result of ``estimate_zone_mass_mult_per_source`` -- a
    ``ZoneLoadEstimate`` (``base``) plus the per-neighbour-column dead time
    that produced it, ``L_by_source[j]`` = the fitted delay (seconds) on the
    path from stepped zone ``j`` to this zone -- data-driven, not the single
    ``plant_sim.OFFDIAG_L_S`` (146.5 s) ASSUMED midpoint every column shared
    before this. ``base.zone``'s own diagonal entry is left at the caller's
    fixed (measured) ``L[zone]``, matching every other estimator in this
    module -- only the off-diagonal (neighbour) columns are searched, since
    those are the ones sec 2/3.8 flagged as ASSUMED rather than measured."""
    base: ZoneLoadEstimate
    L_by_source: dict


def estimate_zone_mass_mult_per_source(
    t: np.ndarray, temps: np.ndarray, duty: np.ndarray, zone: int,
    ambient: float = 20.0, K: Optional[np.ndarray] = None,
    tau_ref: Optional[np.ndarray] = None, L: Optional[np.ndarray] = None,
    min_drive_c: float = 5.0,
    offdiag_candidates_s: Optional[Sequence[float]] = None,
) -> Optional[PerSourceLoadEstimate]:
    """PID_EXPANSION_PLAN.md sec 3.8, the step named but "not yet checked"
    after the flat per-path fix (``L_pair``, 2026-09-03) landed: that fix
    still applies ONE off-diagonal delay (the 135-158 s range's 146.5 s
    ASSUMED midpoint) to every neighbour column alike, even though sec 2
    only ever measured an AGGREGATE range across all six cross-zone paths,
    never a per-pair breakdown. This function grid-searches EACH neighbour
    column's own delay independently against the real data (rather than
    assuming they are all equal), picking -- per column -- whichever
    candidate, combined with the others, maximizes this zone's overall
    regression R^2. This is a search over the model's OWN structure (which
    delay best explains the already-collected samples), not a fit to two
    points of a step response, so it does not inherit the closed-loop
    two-point FOPDT failure mode already shelved for this repo (see
    PID_EXPANSION_PLAN.md's "ramp fit measures ramp rate" note): every
    sample in the window still enters one through-origin OLS regression via
    ``estimate_zone_mass_mult``, exactly as the flat/per-path callers do;
    only the delay APPLIED to each duty column before that regression
    varies across the grid.

    Cost is combinatorial in the neighbour count (``len(offdiag_candidates_s)
    ** (N_ZONES - 1)``) -- fine for this repo's 3-zone case (default 9
    candidates -> 81 combinations per call), not intended for a larger zone
    count without a coarser grid or a smarter search.

    Returns ``None`` under the same conditions ``estimate_zone_mass_mult``
    does (fewer than 3 usable samples at every candidate combination, or a
    non-physical negative/zero fitted slope everywhere).
    """
    K = ps.K_full if K is None else K
    tau_ref = ps.tau if tau_ref is None else tau_ref
    L = ps.L if L is None else L
    n_zones = len(L)
    if offdiag_candidates_s is None:
        offdiag_candidates_s = np.arange(60.0, 220.0, 20.0)  # brackets sec 2's 135-158s range with margin
    neighbours = [j for j in range(n_zones) if j != zone]

    best: Optional[tuple] = None
    for combo in itertools.product(offdiag_candidates_s, repeat=len(neighbours)):
        L_pair = np.tile(np.asarray(L, dtype=float).reshape(-1, 1), (1, n_zones))
        for j, Lc in zip(neighbours, combo):
            L_pair[zone, j] = Lc
        est = estimate_zone_mass_mult(
            t, temps, duty, zone, ambient=ambient, K=K, tau_ref=tau_ref, L=L,
            min_drive_c=min_drive_c, L_pair=L_pair,
        )
        if est is None or np.isnan(est.r2):
            continue
        if best is None or est.r2 > best[0].r2:
            best = (est, dict(zip(neighbours, (float(c) for c in combo))))

    if best is None:
        return None
    est, L_by_source = best
    return PerSourceLoadEstimate(base=est, L_by_source=L_by_source)


def estimate_all_zones(t, temps, duty, ambient=20.0, K=None, tau_ref=None, L=None,
                        min_drive_c=3.0, L_pair=None) -> list:
    out = []
    for z in range(temps.shape[1]):
        est = estimate_zone_mass_mult(t, temps, duty, z, ambient=ambient, K=K,
                                       tau_ref=tau_ref, L=L, min_drive_c=min_drive_c,
                                       L_pair=L_pair)
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


def estimate_per_source_from_capture_path(
    path: str, run_index: int = 0, ambient: float = 20.0, min_drive_c: float = 5.0,
    min_elapsed_s: float = 0.0, offdiag_candidates_s: Optional[Sequence[float]] = None,
) -> list:
    """Convenience wrapper: parse a real capture (either on-disk envelope,
    see ``load_capture_rows``), split multi-run files, take ``run_index``,
    drop the first ``min_elapsed_s`` (dead-time/startup transient), and run
    ``estimate_zone_mass_mult_per_source`` -- one ``PerSourceLoadEstimate``
    per zone whose regression found at least 3 usable samples at some
    candidate combination (zones that never clear that bar are omitted, same
    convention as ``estimate_all_zones``)."""
    rows_all = load_capture_rows(path)
    runs = la.split_runs(rows_all)
    rows = runs[run_index]
    t, temps, duty = arrays_from_capture(rows)
    keep = t >= (t[0] + min_elapsed_s)
    t, temps, duty = t[keep], temps[keep], duty[keep]
    out = []
    for z in range(temps.shape[1]):
        est = estimate_zone_mass_mult_per_source(
            t, temps, duty, z, ambient=ambient, min_drive_c=min_drive_c,
            offdiag_candidates_s=offdiag_candidates_s,
        )
        if est is not None:
            out.append(est)
    return out
