"""Offline coupled-identification validator for the 3-zone kiln coupling
matrix -- closes the gap documented in
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.2/3.3: the firmware's
coupled least-squares solve (``adaptive_tune_model.c``'s ``coupled_fit()``)
is proven CORRECT against a synthetic matrix, but has never been proven
BETTER than the matrix it would replace, on real dwell data.

Nothing here touches the board. It is pure offline computation over
``log_analysis.py``'s ``PollRow``/``ZoneSample`` dataclasses (reused, not
re-parsed -- see that module's own "SOURCE KINDS" note) from a
``/api/profile_exec`` poll capture.

ORIENTATION -- read this before touching anything below. The persisted
storage convention (``zones_config_get/set_coupling()``) is
``coupling_coeff[affected][stepped]``: row = the zone whose temperature is
affected, column = the zone whose duty was stepped. ``plant_sim.K_full`` is
already in this orientation (see its own comment -- built by transposing
its ``_wire_form[stepped][affected]`` bench data) and is reused here
UNCHANGED as "the current on-board matrix" rather than re-declaring the
same numbers a second time. The coupled steady-state relation this module
solves and scores is ``A @ u = (T - ambient)`` in that same
``[affected][stepped]`` orientation -- ``u_pred = A^-1 @ (T - ambient)``.
Never the transpose (that's the HTTP surface's ``/api/autotune/matrix``
convention, which this module never touches).

DWELL = A DC-GAIN MEASUREMENT. During a dwell, ``target_rate_c_per_s`` is
zeroed every tick and only the ramp branch of ``profile_executor.c`` writes
it, so a dwell's steady duty is pure hold term -- no climb feedforward, no
extrapolation. A settled dwell therefore gives one exact, noise-limited
observation per zone: ``K_row . u_settled = T_settled - ambient``.

SETTLE DEFINITION -- deliberately matches the firmware's own dwell-
harvesting settle detector (``adaptive_tune.c``,
``ADAPTIVE_TUNE_SETTLE_MIN_S`` / ``ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S``
/ ``ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION`` in
``adaptive_tune_internal.h``): a zone is settled once at least 180 s have
elapsed since the first valid tick of this dwell AND
``|actual_c - actual_c_at_dwell_start| / elapsed_s <= 0.003 C/s``, and the
observation is discarded (not just deferred) if duty at that instant is
below 0.03. Reusing the exact firmware thresholds, rather than inventing a
PC-side tolerance, is what makes this module's numbers comparable to what
the board itself would harvest.

JOINT (all-zone) OBSERVATIONS -- the coupled solve needs a duty VECTOR
(all three zones at once), not one zone's scalar reading. The firmware
(``adaptive_tune.c``'s joint-row commit) requires every zone to have a
FRESH, valid, above-floor duty reading at the exact tick one zone crosses
its own settle test. This module cannot reproduce that tick-exact
bookkeeping from a poll capture (no per-tick "freshness" state survives
into the log), so it uses a documented, slightly looser substitute: when
zone i crosses its own settle test at row R, the joint observation pairs
zone i's actual_c at R with EVERY zone's (actual_c, duty) AT THAT SAME ROW
R -- whether or not each peer zone has independently crossed its own
settle test yet. A dwell's duty is expected to be close to flat once any
one zone has settled (that is what "settled" means), so peers a few polls
away from crossing their own threshold contribute a duty reading that is
very close to, but not guaranteed identical to, their own eventual settled
value. This trades a small amount of per-row fidelity for using far more
of the real capture data than the strict joint-tick rule would keep, and
is why this module's own self-check (``self_check_against_known_figures``)
reports its numbers as an approximate, not exact, reproduction of the
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.2 figures.

AMBIENT REFERENCE -- these poll captures never carry the firmware's own
cold-junction-derived ambient reading. Following sec 3.2's own
methodology, this module uses each RUN's own first sample as that zone's
ambient reference (the kiln is cold at the start of every one of these
captures) -- a single scalar per zone per run, not resampled per dwell.

CONDITIONING -- the crux of this whole module. If every zone tracks the
same setpoint (which every profile in these fixtures does), the duty
vectors across dwells are collinear or near-collinear: the ratio between
zones' duty stays roughly fixed and only the OVERALL level moves with
temperature. No amount of such data determines the off-diagonal terms --
the system is genuinely underdetermined, not just noisy. This module
reports the observation duty matrix's own condition number
(``numpy.linalg.cond``) and REFUSES to produce a fit above
``COND_REFUSAL_THRESHOLD`` (1e4, mirroring the firmware's own admitted
bound -- see ``zone_coupling_solve.h``'s ``COUPLING_SOLVE_PIVOT_REL_EPS``
comment, a 1e-4 relative pivot floor that admits condition numbers up to
about 1e4). A confident matrix fit from collinear data is the specific
failure mode this module exists to avoid, not a corner case to be
tolerated for the sake of always returning an answer.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
from typing import Optional, Sequence

import numpy as np

from . import log_analysis
from . import plant_sim

ZONES = (0, 1, 2)

# Firmware constants -- see adaptive_tune_internal.h. Kept as named module
# constants (not re-derived) so a firmware change to these thresholds is a
# one-line diff here too, and so a test can flip one in isolation.
SETTLE_MIN_S = 180.0
SETTLE_SLOPE_FLOOR_C_PER_S = 0.003
MIN_DUTY_FOR_OBSERVATION = 0.03

# This module's own conditioning bound -- see module docstring's
# "CONDITIONING" section for why this specific value.
COND_REFUSAL_THRESHOLD = 1.0e4

# The matrix currently on the board, reused verbatim (not re-declared) from
# plant_sim.py -- see that module's own comment on _wire_form/K_full for its
# [affected][stepped] orientation and bench-measurement provenance.
CURRENT_MATRIX = plant_sim.K_full


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class JointObservation:
    """One settled-dwell reading of all three zones at once: duty vector
    ``u``, temperature vector ``T``, and the ``ambient`` vector each T is
    measured against (see module docstring's "AMBIENT REFERENCE"). All
    three arrays are ordered by ``ZONES`` (0, 1, 2).
    """
    T: np.ndarray
    ambient: np.ndarray
    u: np.ndarray
    settled_zone: int      # which zone's own settle test produced this row
    dwell_target_c: float  # the segment's target_c, for the nonlinearity report
    source: str
    elapsed_s: float

    @property
    def rise(self) -> np.ndarray:
        return self.T - self.ambient


@dataclasses.dataclass
class CoupledFitResult:
    matrix: Optional[np.ndarray]   # [affected][stepped], or None if refused
    condition_number: float
    n_observations: int
    refused: bool
    reason: str


@dataclasses.dataclass
class ZoneScore:
    zone: int
    n: int
    mean_error: float   # mean(u_pred - u_actual)
    rms_error: float


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

def _run_ambient(rows: Sequence[log_analysis.PollRow]) -> Optional[dict]:
    """This run's per-zone ambient reference: the first row carrying zone
    data (see module docstring's "AMBIENT REFERENCE")."""
    for r in rows:
        if r.zones:
            return {z: s.actual_c for z, s in r.zones.items()}
    return None


def _zone_settle_row(rows: Sequence[log_analysis.PollRow], zone: int,
                      min_s: float = SETTLE_MIN_S,
                      require_min_duty: bool = True,
                      ) -> Optional[log_analysis.PollRow]:
    """The first row within ``rows`` (already sliced to one dwell window)
    at which ``zone`` crosses the firmware's own settle test, or ``None``
    if it never does (window too short, missing/NaN data, or a genuine
    still-drifting zone -- see module docstring's "SETTLE DEFINITION").
    Mirrors ``adaptive_tune.c``'s dwell-settle state machine line for line:
    the settle window starts at the first valid tick, never resets on a
    slope failure (only a fresh dwell resets it), and the observation is
    discarded outright (returns ``None``, not deferred) if duty is below
    the firmware's own floor at the settle instant, UNLESS
    ``require_min_duty`` is False -- see
    ``single_zone_column_observation`` for why a PASSIVE zone in a
    single-zone excitation firing must be exempted from that floor (it is
    supposed to be at ~0 duty; that is the whole point of the experiment)
    while still applying the same slope-settle logic, just with a much
    longer ``min_s`` (cross-zone tau is 620-730 s, roughly 4x a directly-
    heated zone's own dynamics -- the caller is responsible for passing an
    ``min_s`` long enough for THIS row's own dynamics, not this function's
    default).
    """
    settle_start_c: Optional[float] = None
    settle_start_s: Optional[float] = None
    for r in rows:
        zs = r.zones.get(zone)
        if zs is None or math.isnan(zs.actual_c):
            continue
        if settle_start_c is None:
            settle_start_c = zs.actual_c
            settle_start_s = r.elapsed_s
        elapsed = r.elapsed_s - settle_start_s
        if elapsed < min_s:
            continue
        slope = abs(zs.actual_c - settle_start_c) / elapsed if elapsed > 0 else 0.0
        if slope > SETTLE_SLOPE_FLOOR_C_PER_S:
            continue  # still drifting -- keep waiting, do not reset
        if require_min_duty and zs.duty < MIN_DUTY_FOR_OBSERVATION:
            return None  # real steady state, but too little duty to trust
        return r
    return None


def dwell_observations_for_run(rows: Sequence[log_analysis.PollRow], source: str = ""
                                ) -> list[JointObservation]:
    """Extract every joint dwell observation from a SINGLE run (already
    split with ``log_analysis.split_runs`` -- see that module's own
    multi-run hazard note; this function does not split for you)."""
    ambient = _run_ambient(rows)
    if ambient is None or any(z not in ambient for z in ZONES):
        return []
    ambient_vec = np.array([ambient[z] for z in ZONES])

    obs: list[JointObservation] = []
    for w in log_analysis.build_windows(rows):
        if w.phase != "dwell":
            continue
        window_rows = rows[w.start_idx:w.end_idx + 1]
        target_c = window_rows[-1].target_c
        for zone in ZONES:
            settle_row = _zone_settle_row(window_rows, zone)
            if settle_row is None:
                continue
            if any(z not in settle_row.zones for z in ZONES):
                continue
            if any(math.isnan(settle_row.zones[z].actual_c) for z in ZONES):
                continue
            T = np.array([settle_row.zones[z].actual_c for z in ZONES])
            u = np.array([settle_row.zones[z].duty for z in ZONES])
            obs.append(JointObservation(
                T=T, ambient=ambient_vec, u=u, settled_zone=zone,
                dwell_target_c=target_c, source=source,
                elapsed_s=settle_row.elapsed_s,
            ))
    return obs


def dwell_observations_from_paths(paths: Sequence[str]) -> list[JointObservation]:
    """Extract joint dwell observations from one or more capture files,
    each possibly containing more than one run (split via
    ``log_analysis.split_runs``) and possibly still being written to (a
    truncated trailing line from ``parse_profile_exec_jsonl`` is simply
    skipped, same tolerance that module documents for a flaky-link
    capture) -- safe to point at a live-firing log mid-run.
    """
    all_obs: list[JointObservation] = []
    for path in paths:
        rows_all = log_analysis.parse_profile_exec_jsonl(path)
        for run_idx, rows in enumerate(log_analysis.split_runs(rows_all)):
            if not any(r.zones for r in rows):
                continue
            label = path if run_idx == 0 else f"{path}#run{run_idx}"
            all_obs.extend(dwell_observations_for_run(rows, source=label))
    return all_obs


# ---------------------------------------------------------------------------
# Single-zone excitation -- the clean experiment, not a solve at all.
#
# Every path above shares one structural limit: it observes ORDINARY
# firings, where every zone tracks the same setpoint, so the duty vectors
# it has to work with are (near-)collinear by construction (see module
# docstring's CONDITIONING section) -- no amount of MORE ordinary dwell
# data fixes that, because the data is redundant, not merely noisy.
#
# A single-zone firing (one zone heated, the other two passive) sidesteps
# the whole determinability question: column j of the matrix IS
# ``dT_i / u_j`` at steady state, directly, with no linear system to
# solve and nothing to be collinear WITH (there is only one nonzero duty).
# The firmware's own joint-observation harvest cannot use this data (its
# joint-row commit requires every zone's duty above
# ``ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION`` -- see adaptive_tune.c -- and
# a passive zone by design never clears that), but nothing stops this
# offline module from reading it out of a capture directly.
#
# The passive zones settle far more slowly than the active one: cross-zone
# tau is 620-730 s and cross dead time 135-158 s (vs the actively-heated
# zone's own, much faster, direct dynamics), so a passive zone needs on
# the order of 30+ minutes of dwell before its own settle test can fire --
# SINGLE_ZONE_PASSIVE_SETTLE_MIN_S below is set to that, not to
# SETTLE_MIN_S. Using the short active-zone threshold for a passive zone
# would accept it as "settled" while it is still most of the way through
# its own step response.
# ---------------------------------------------------------------------------

SINGLE_ZONE_PASSIVE_SETTLE_MIN_S = 1800.0  # >= 30 min, see comment above


@dataclasses.dataclass
class ColumnObservation:
    """One entry of column ``active_zone`` of the matrix, measured
    directly from a single-zone firing: ``k = rise_c / active_duty`` for
    zone ``affected_zone`` (``affected_zone == active_zone`` is the
    diagonal entry, measured the ordinary settled way; any other
    ``affected_zone`` is a passive-zone cross-coupling entry, measured
    with the longer passive settle window).
    """
    active_zone: int
    affected_zone: int
    active_duty: float
    rise_c: float          # affected zone's (actual_c - ambient) at ITS settle instant
    k: float                # rise_c / active_duty -- the measured coefficient
    source: str
    elapsed_s: float


def single_zone_column_observations(rows: Sequence[log_analysis.PollRow], active_zone: int,
                                     source: str = "") -> list[ColumnObservation]:
    """Extract column ``active_zone`` observations from a single run of a
    single-zone firing (one zone heated, the other two passive). Does NOT
    check ``zone_mask`` (not carried by ``PollRow`` -- see
    ``log_analysis.py``); instead it infers "which zone was driven" from
    the data itself at each dwell window: the active zone must clear the
    ordinary settle test (short window, above-floor duty); a zone that
    never rises meaningfully above its own ambient is simply not usable as
    a passive-zone observation for that window (rather than assumed to be
    "0 rise, 0 coefficient" -- a passive zone with negligible duty AND
    negligible rise gives a 0/small-number ratio that is noise, not a
    measurement, and is skipped).
    """
    ambient = _run_ambient(rows)
    if ambient is None or active_zone not in ambient:
        return []
    obs: list[ColumnObservation] = []
    for w in log_analysis.build_windows(rows):
        if w.phase != "dwell":
            continue
        window_rows = rows[w.start_idx:w.end_idx + 1]

        active_row = _zone_settle_row(window_rows, active_zone,
                                       min_s=SETTLE_MIN_S, require_min_duty=True)
        if active_row is None:
            continue
        active_zs = active_row.zones.get(active_zone)
        active_duty = active_zs.duty
        if active_duty < MIN_DUTY_FOR_OBSERVATION:
            continue

        for affected in ZONES:
            amb = ambient.get(affected)
            if amb is None:
                continue
            if affected == active_zone:
                settle_row = active_row
            else:
                settle_row = _zone_settle_row(window_rows, affected,
                                               min_s=SINGLE_ZONE_PASSIVE_SETTLE_MIN_S,
                                               require_min_duty=False)
            if settle_row is None:
                continue
            zs = settle_row.zones.get(affected)
            if zs is None or math.isnan(zs.actual_c):
                continue
            rise = zs.actual_c - amb
            if rise <= 0.05:
                continue  # no meaningfully measurable rise -- see docstring
            obs.append(ColumnObservation(
                active_zone=active_zone, affected_zone=affected,
                active_duty=active_duty, rise_c=rise, k=rise / active_duty,
                source=source, elapsed_s=settle_row.elapsed_s,
            ))
    return obs


def single_zone_column_observations_from_paths(paths: Sequence[str], active_zone: int
                                                 ) -> list[ColumnObservation]:
    """Same file/run handling as ``dwell_observations_from_paths``, for a
    set of captures known to be single-zone firings driving ``active_zone``.
    """
    all_obs: list[ColumnObservation] = []
    for path in paths:
        rows_all = log_analysis.parse_profile_exec_jsonl(path)
        for run_idx, rows in enumerate(log_analysis.split_runs(rows_all)):
            if not any(r.zones for r in rows):
                continue
            label = path if run_idx == 0 else f"{path}#run{run_idx}"
            all_obs.extend(single_zone_column_observations(rows, active_zone, source=label))
    return all_obs


def matrix_from_single_zone_columns(column_obs_by_active_zone: dict) -> tuple[Optional[np.ndarray], dict]:
    """Assemble a full matrix directly from up to three single-zone
    firings' column observations (``{active_zone: [ColumnObservation, ...]}``,
    one list per driven zone). Each cell is the MEAN of that cell's
    ``k`` across every observation that measured it -- no linear algebra,
    because there is nothing to solve: each single-zone firing measures
    one column directly and independently of the others.

    Returns ``(matrix, coverage)`` -- ``matrix`` is ``None`` if ANY of the
    9 cells has zero observations (an incomplete matrix is refused rather
    than silently left at some fallback value); ``coverage`` is always
    returned (``{(affected, active): n_observations}``) so a caller can
    see exactly which firings are still needed.
    """
    coverage: dict = {}
    cells: dict = {}
    for active_zone, obs_list in column_obs_by_active_zone.items():
        for o in obs_list:
            key = (o.affected_zone, o.active_zone)
            cells.setdefault(key, []).append(o.k)
    for i in ZONES:
        for j in ZONES:
            coverage[(i, j)] = len(cells.get((i, j), []))

    if any(coverage[(i, j)] == 0 for i in ZONES for j in ZONES):
        return None, coverage

    matrix = np.zeros((3, 3))
    for i in ZONES:
        for j in ZONES:
            matrix[i, j] = float(np.mean(cells[(i, j)]))
    return matrix, coverage


# ---------------------------------------------------------------------------
# Solve
# ---------------------------------------------------------------------------

def solve_coupled(observations: Sequence[JointObservation]) -> CoupledFitResult:
    """Least-squares fit of the full 3x3 A in ``A @ u = (T - ambient)``,
    one independent row-solve per affected zone against the SAME duty
    design matrix (mirrors ``adaptive_tune_model.c``'s ``coupled_fit()``:
    "built once and n independent vector solves are run against it").
    Refuses -- returns ``matrix=None`` -- when there are too few
    observations to determine 3 unknowns per row, or when the observation
    duty matrix is too collinear (see module docstring's "CONDITIONING").
    """
    n = len(observations)
    cond = float("inf")
    if n >= 1:
        U = np.array([o.u for o in observations])
        try:
            cond = float(np.linalg.cond(U))
        except np.linalg.LinAlgError:
            cond = float("inf")

    if n < 3:
        return CoupledFitResult(None, cond, n, True,
                                 f"only {n} joint observation(s) -- need at least 3 to determine a 3x3 system")

    if not math.isfinite(cond) or cond > COND_REFUSAL_THRESHOLD:
        return CoupledFitResult(
            None, cond, n, True,
            f"observation duty matrix is too collinear (condition number "
            f"{cond:.3g} exceeds {COND_REFUSAL_THRESHOLD:.0e}) -- the "
            f"off-diagonal terms are not determined by this data, not just "
            f"noisy in it")

    U = np.array([o.u for o in observations])
    A_fit = np.zeros((3, 3))
    for i in ZONES:
        rhs = np.array([o.rise[i] for o in observations])
        row, *_ = np.linalg.lstsq(U, rhs, rcond=None)
        A_fit[i, :] = row

    return CoupledFitResult(A_fit, cond, n, False, "")


def matrix_plausibility(matrix: np.ndarray) -> tuple[bool, str]:
    """A physical sanity check the raw condition-number gate above does
    NOT perform: every entry of a real coupling matrix must be
    non-negative -- more duty on any zone can only add heat, never remove
    it, so a fitted entry going negative is not "a small off-diagonal",
    it is a sign that the fit is unconstrained noise, not a measurement.

    This is deliberately a SEPARATE check from ``solve_coupled``'s
    conditioning refusal, not folded into it: a fit can pass the ~1e4
    condition-number gate (nominally "determined") and still come out
    physically nonsensical on real same-setpoint dwell data, because the
    duty vectors being merely non-singular is a much weaker property than
    the duty vectors actually spanning the space independently enough to
    pin down three unknowns per row against real noise. Surfacing both
    numbers separately, rather than raising the condition-number threshold
    until this stops happening, is what makes the gap between "solvable in
    principle" and "trustworthy in practice" visible instead of hidden
    behind a single pass/fail bit.
    """
    reasons = []
    if np.any(matrix < 0):
        bad = [(i, j) for i in ZONES for j in ZONES if matrix[i, j] < 0]
        reasons.append(f"{len(bad)} negative entr{'y' if len(bad) == 1 else 'ies'} "
                        f"(zone pairs {bad}) -- a coupling coefficient cannot be negative")

    # Weaker, more assumption-laden check, reported separately in the same
    # reason string: on the bench-measured current matrix, every zone's
    # OWN duty affects its OWN temperature more than any neighbor's duty
    # does (each row's diagonal is its largest entry) -- physically
    # expected for zones that are not perfectly thermally merged. A fit
    # that puts a zone's largest sensitivity on a NEIGHBOR's duty instead
    # of its own is a strong (not certain) sign of overfitting collinear
    # data rather than a real measurement.
    non_diag_dominant_rows = [
        i for i in ZONES
        if matrix[i, i] < max(matrix[i, j] for j in ZONES if j != i)
    ]
    if non_diag_dominant_rows:
        reasons.append(f"row(s) {non_diag_dominant_rows} are not diagonal-dominant (a zone's own duty "
                        f"is not its largest sensitivity) -- physically expected for the current matrix, "
                        f"a strong sign of overfitting on this fit")

    if reasons:
        return False, "; ".join(reasons)
    return True, ""


# ---------------------------------------------------------------------------
# Score
# ---------------------------------------------------------------------------

def score_matrix(matrix: np.ndarray, observations: Sequence[JointObservation]
                  ) -> list[ZoneScore]:
    """Score ANY candidate matrix (``[affected][stepped]``) against
    observations: per-zone mean and RMS of ``u_pred - u_actual``, where
    ``u_pred = matrix^-1 @ (T - ambient)``. This is exactly the figure
    ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.2 reports for the
    current on-board matrix (-0.086 / -0.007 / +0.108) -- pass
    ``CURRENT_MATRIX`` to reproduce it (approximately -- see
    ``self_check_against_known_figures``) as a pipeline sanity check
    before trusting a score for a NEW candidate matrix.
    """
    inv = np.linalg.inv(matrix)
    errors = {z: [] for z in ZONES}
    for o in observations:
        u_pred = inv @ o.rise
        err = u_pred - o.u
        for i in ZONES:
            errors[i].append(err[i])
    out = []
    for z in ZONES:
        a = np.array(errors[z]) if errors[z] else np.array([0.0])
        out.append(ZoneScore(
            zone=z, n=len(errors[z]),
            mean_error=float(a.mean()) if errors[z] else float("nan"),
            rms_error=float(np.sqrt((a ** 2).mean())) if errors[z] else float("nan"),
        ))
    return out


# ---------------------------------------------------------------------------
# Nonlinearity report
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class NonlinearityPoint:
    zone: int
    dwell_target_c: float
    error: float  # u_pred - u_actual, current on-board matrix


def nonlinearity_report(observations: Sequence[JointObservation],
                         matrix: Optional[np.ndarray] = None) -> list[NonlinearityPoint]:
    """Per-zone prediction error as a function of dwell temperature --
    makes the sec 3.2 "z2's error GROWS with dwell temperature" signature
    visible, and extensible to captures well outside the 45/60 C range
    those figures came from (e.g. the 30-70 C coupid6 sweep).
    """
    m = matrix if matrix is not None else CURRENT_MATRIX
    inv = np.linalg.inv(m)
    points = []
    for o in observations:
        u_pred = inv @ o.rise
        err = u_pred - o.u
        for i in ZONES:
            points.append(NonlinearityPoint(zone=i, dwell_target_c=o.dwell_target_c, error=float(err[i])))
    points.sort(key=lambda p: (p.zone, p.dwell_target_c))
    return points


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

# firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.2: mean(u_pred - u_actual)
# across 12 dwells x 3 zones (baseline/after/ifix/holdfix_clean/final +
# track3zone_easeoff), current on-board matrix.
KNOWN_FIGURES_MEAN = {0: -0.086, 1: -0.007, 2: 0.108}
# How close this module's own extraction pipeline is required to land to
# call the self-check a pass -- see module docstring's "JOINT (all-zone)
# OBSERVATIONS" section for why an EXACT reproduction is not expected: this
# module's joint-row substitute is a documented approximation of a firmware
# tick-exact rule this module cannot see from a poll capture, and one of
# the six original captures (track3zone_easeoff) is not present in this
# repo's fixtures. Same sign and same rough magnitude on all three zones is
# the bar; anything else means the pipeline is wrong, not just approximate.
SELF_CHECK_TOLERANCE_C = 0.06


def self_check_against_known_figures(paths: Sequence[str]) -> dict:
    """Reproduce (approximately -- see module docstring) sec 3.2's
    -0.086/-0.007/+0.108 figures for the current on-board matrix, as a
    sanity check that this module's extraction pipeline is measuring the
    same thing that analysis did. Returns a dict with pass/fail per zone
    and the observed vs known mean error.
    """
    obs = dwell_observations_from_paths(paths)
    scores = score_matrix(CURRENT_MATRIX, obs)
    results = {}
    all_ok = True
    for s in scores:
        known = KNOWN_FIGURES_MEAN[s.zone]
        same_sign = (known == 0) or (s.mean_error == 0) or (math.copysign(1, known) == math.copysign(1, s.mean_error))
        close = abs(s.mean_error - known) <= SELF_CHECK_TOLERANCE_C
        ok = same_sign and close
        all_ok = all_ok and ok
        results[s.zone] = dict(known=known, observed=round(s.mean_error, 4), n=s.n, ok=ok)
    return dict(ok=all_ok, n_observations=len(obs), zones=results)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def render_report(paths: Sequence[str]) -> dict:
    obs = dwell_observations_from_paths(paths)
    current_scores = score_matrix(CURRENT_MATRIX, obs) if obs else []
    fit = solve_coupled(obs)
    fit_scores = score_matrix(fit.matrix, obs) if (fit.matrix is not None and obs) else None
    fit_plausible, fit_plausibility_reason = (
        matrix_plausibility(fit.matrix) if fit.matrix is not None else (None, "")
    )
    nonlin = nonlinearity_report(obs, CURRENT_MATRIX)
    self_check = self_check_against_known_figures(paths)

    return dict(
        paths=list(paths),
        n_observations=len(obs),
        current_matrix=CURRENT_MATRIX.tolist(),
        current_scores=[dataclasses.asdict(s) for s in current_scores],
        # HEADLINE FIGURE -- see module docstring's "CONDITIONING" section.
        # A number well under the refusal threshold means "not literally
        # singular", not "reliably determined" -- always read this
        # together with fit_plausible below, not in isolation.
        fit_condition_number=fit.condition_number,
        fit_refused=fit.refused,
        fit_refusal_reason=fit.reason,
        fit_matrix=(fit.matrix.tolist() if fit.matrix is not None else None),
        fit_scores=([dataclasses.asdict(s) for s in fit_scores] if fit_scores is not None else None),
        fit_plausible=fit_plausible,
        fit_plausibility_reason=fit_plausibility_reason,
        nonlinearity=[dataclasses.asdict(p) for p in nonlin],
        self_check=self_check,
    )


def format_report_text(report: dict) -> str:
    lines = [f"coupled-identification report: {len(report['paths'])} file(s), "
             f"{report['n_observations']} joint dwell observation(s)"]
    lines.append("")
    lines.append(f"*** observation duty matrix condition number: {report['fit_condition_number']:.3g} "
                 f"(refusal threshold {COND_REFUSAL_THRESHOLD:.0e}) ***")
    lines.append("    a pass here means 'not literally singular', NOT 'reliably determined' --")
    lines.append("    read together with the physical-plausibility check on the fitted matrix below.")
    lines.append("")
    lines.append("current on-board matrix [affected][stepped]:")
    for row in report["current_matrix"]:
        lines.append("  " + " ".join(f"{v:8.3f}" for v in row))
    if report["current_scores"]:
        lines.append("current matrix score (mean / rms of u_pred - u_actual):")
        for s in report["current_scores"]:
            lines.append(f"  zone{s['zone']}: n={s['n']:3d} mean={s['mean_error']:+.3f} rms={s['rms_error']:.3f}")
    else:
        lines.append("current matrix score: no observations extracted")
    lines.append("")
    if report["fit_refused"]:
        lines.append(f"fit REFUSED: {report['fit_refusal_reason']}")
    else:
        lines.append("fitted matrix [affected][stepped]:")
        for row in report["fit_matrix"]:
            lines.append("  " + " ".join(f"{v:8.3f}" for v in row))
        lines.append("fitted matrix score (mean / rms of u_pred - u_actual):")
        for s in report["fit_scores"]:
            lines.append(f"  zone{s['zone']}: n={s['n']:3d} mean={s['mean_error']:+.3f} rms={s['rms_error']:.3f}")
        lines.append(f"physical plausibility: {'PASS' if report['fit_plausible'] else 'FAIL'}"
                     + (f" -- {report['fit_plausibility_reason']}" if not report['fit_plausible'] else ""))
    lines.append("")
    lines.append(f"self-check vs PID_EXPANSION_PLAN.md sec 3.2 figures: "
                 f"{'PASS' if report['self_check']['ok'] else 'FAIL'} "
                 f"(n={report['self_check']['n_observations']})")
    for zone, r in sorted(report["self_check"]["zones"].items()):
        lines.append(f"  zone{zone}: known={r['known']:+.3f} observed={r['observed']:+.3f} "
                     f"n={r['n']} {'ok' if r['ok'] else 'MISMATCH'}")
    return "\n".join(lines)


def format_single_zone_report_text(matrix, coverage: dict, scores: Optional[list[ZoneScore]]) -> str:
    lines = ["single-zone-excitation matrix [affected][stepped] -- each cell measured"
             " directly (dT_i/u_j), no linear solve:"]
    lines.append("coverage (n observations per cell):")
    for i in ZONES:
        lines.append("  " + " ".join(f"{coverage.get((i, j), 0):3d}" for j in ZONES))
    if matrix is None:
        missing = [(i, j) for i in ZONES for j in ZONES if coverage.get((i, j), 0) == 0]
        lines.append(f"matrix INCOMPLETE -- missing cells {missing}; refusing to fill with a fallback value")
        return "\n".join(lines)
    lines.append("matrix:")
    for row in matrix.tolist():
        lines.append("  " + " ".join(f"{v:8.3f}" for v in row))
    plausible, reason = matrix_plausibility(matrix)
    lines.append(f"physical plausibility: {'PASS' if plausible else 'FAIL'}" + (f" -- {reason}" if not plausible else ""))
    if scores is not None:
        lines.append("score against joint dwell observations (mean / rms of u_pred - u_actual):")
        for s in scores:
            lines.append(f"  zone{s.zone}: n={s.n:3d} mean={s.mean_error:+.3f} rms={s.rms_error:.3f}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-coupled-ident",
        description="offline coupled-identification validator for the kiln coupling matrix",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_report = sub.add_parser("report", help="fit + score + nonlinearity + self-check over one or more captures")
    p_report.add_argument("jsonl_paths", nargs="+")
    p_report.add_argument("--json", action="store_true")

    p_sz = sub.add_parser("single-zone", help="assemble a matrix directly from up to three single-zone-excitation "
                                                "captures (one zone driven, other two passive)")
    p_sz.add_argument("--zone0", nargs="*", default=[], help="capture(s) with zone 0 driven alone")
    p_sz.add_argument("--zone1", nargs="*", default=[], help="capture(s) with zone 1 driven alone")
    p_sz.add_argument("--zone2", nargs="*", default=[], help="capture(s) with zone 2 driven alone")
    p_sz.add_argument("--score-against", nargs="*", default=[],
                       help="ordinary poll captures to score the assembled matrix against (joint dwell observations)")
    p_sz.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "report":
        report = render_report(args.jsonl_paths)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print(format_report_text(report))
    elif args.cmd == "single-zone":
        by_zone = {0: args.zone0, 1: args.zone1, 2: args.zone2}
        column_obs = {
            z: single_zone_column_observations_from_paths(paths, z)
            for z, paths in by_zone.items() if paths
        }
        matrix, coverage = matrix_from_single_zone_columns(column_obs)
        scores = None
        if matrix is not None and args.score_against:
            joint_obs = dwell_observations_from_paths(args.score_against)
            scores = score_matrix(matrix, joint_obs) if joint_obs else None
        if args.json:
            out = dict(
                matrix=(matrix.tolist() if matrix is not None else None),
                coverage={f"{i},{j}": n for (i, j), n in coverage.items()},
                scores=([dataclasses.asdict(s) for s in scores] if scores is not None else None),
            )
            print(json.dumps(out, indent=2))
        else:
            print(format_single_zone_report_text(matrix, coverage, scores))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
