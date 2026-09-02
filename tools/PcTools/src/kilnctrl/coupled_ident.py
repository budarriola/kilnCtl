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

WHAT ORDINARY-FIRING DWELLS CANNOT TELL YOU (2026-09-02, coupid6). A
constant-A model makes a clean, checkable prediction for a dwell where
every zone is commanded to the same setpoint: the DUTY RATIO between
zones should be independent of temperature (only the overall magnitude
scales). An early pass at testing this against the coupid6 capture (10
min dwells, three temperatures) appeared to show that ratio spreading
with temperature -- but that read was WRONG, and the conclusion was
retracted: those dwells run for ~600 s against a ~265 s zone tau, i.e.
only ~2.3 tau, and the plant is visibly under-damped at these gains
(zone 0's duty swung 0.023 -> 0.19 -> 0.144 within a single dwell window).
The apparent "ratio changes with temperature" was two different points on
a still-moving trajectory being compared as if they were two steady
states -- not evidence about the matrix at all. See
``settle_criterion_audit`` below for what this specifically implies about
the SETTLE TEST: it only watches ``actual_c``'s slope, so it can and did
fire at a coincidental flat spot mid-oscillation while duty was still
swinging by up to 23% of its own value for the rest of that same window.
Every "settled" observation in the coupid6 capture is flagged by that
audit. No conclusion about the coupling matrix's linearity should be
drawn from ordinary-firing dwell data until the plant is unambiguously at
steady state; the single-zone excitation captures (35 min dwells, ~8 tau
for the driven zone) are the ones designed to actually get there -- see
``single_zone_column_observations`` above.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
from typing import Optional, Sequence

import numpy as np

from . import coupling_pair_log
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
    # 'confirmed': passed a real settle test (firmware thresholds, or a
    # manual capture with >=2 same-target samples showing a small slope
    # between the last two). 'unconfirmed': accepted anyway (e.g. a single
    # manual sample with no second reading to check slope against) --
    # callers doing anything conclusion-sensitive (direction_constancy_
    # report in particular) should treat 'unconfirmed' points as a
    # caveat, not silently equal evidence. Defaults to 'confirmed' so
    # every existing producer of this dataclass (which already only
    # emits real firmware-settle-tested rows) is unaffected.
    settled_confidence: str = "confirmed"

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


# ---------------------------------------------------------------------------
# Settle-criterion audit -- does the firmware's own dwell-settle test
# (SETTLE_MIN_S / SETTLE_SLOPE_FLOOR_C_PER_S, mirrored by _zone_settle_row
# above) actually catch a zone at steady state, or can it fire mid-
# oscillation on a dwell that is too short relative to the plant's tau?
#
# This question came directly out of the coupid6 capture (10-minute
# dwells against a ~265 s tau, i.e. only ~2.3 tau -- a first-order step is
# ~90% converged there, but this plant is visibly UNDER-DAMPED at these
# gains: zone 0's duty during the 46 C dwell swings 0.023 -> 0.19 -> 0.144
# over the window, oscillating around the setpoint rather than settling
# monotonically. The settle test only looks at ACTUAL_C's slope over its
# own window; nothing about it inspects DUTY at all, so it is entirely
# possible for the slope test to pass at a moment that is, by coincidence,
# a local flat spot in an oscillation -- while duty is still swinging
# across the rest of the same dwell. A DC-gain reading taken there is not
# a DC-gain reading; it is one sample of a limit cycle mislabeled as
# steady state, and every downstream fit (this module's, and the
# firmware's own adaptive_tune.c harvest) inherits that bias silently.
#
# This audit does NOT change what dwell_observations_for_run() extracts --
# it is a diagnostic on the side, meant to be read before trusting any
# observation this module (or the firmware) produced from a short dwell.
# ---------------------------------------------------------------------------

# How much a zone's duty is allowed to range over the REST of a dwell
# window (after its own settle instant fires) before that "settled"
# reading is flagged as suspect. Not derived from firmware behavior (the
# firmware has no equivalent check) -- chosen as roughly the same scale as
# MIN_DUTY_FOR_OBSERVATION itself: a duty swing bigger than the floor that
# decides whether a reading is trustworthy in the first place is not a
# small ripple.
UNSTABLE_DUTY_RANGE_ABS = 0.05
# ... or, for a zone running at higher duty, a swing that is a large
# FRACTION of the settled duty itself (an oscillation of 0.05 around a
# duty of 0.6 is much less alarming than the same swing around 0.1).
UNSTABLE_DUTY_RANGE_FRAC = 0.25


@dataclasses.dataclass
class SettleAuditEntry:
    zone: int
    dwell_target_c: float
    window_duration_s: float
    settled: bool
    settle_elapsed_s: Optional[float]
    settle_duty: Optional[float]
    window_duty_min: Optional[float]
    window_duty_max: Optional[float]
    window_duty_range: Optional[float]
    flagged_unstable: bool
    source: str


def dwell_window_duty_range(window_rows: Sequence[log_analysis.PollRow], zone: int,
                             from_elapsed_s: float = 0.0) -> Optional[tuple]:
    """(min, max) duty for ``zone`` across every valid row in
    ``window_rows`` from ``from_elapsed_s`` ONWARD -- deliberately not the
    whole window: the early rows right after a dwell is entered legitimately
    swing a lot (the PID is still converging from the ramp), and including
    that stretch would flag almost every settle reading as unstable
    regardless of what happens afterward. Passing the settle instant's own
    ``elapsed_s`` here checks the thing this audit actually cares about --
    whether duty was still moving AFTER the settle test said "done".
    """
    duties = [r.zones[zone].duty for r in window_rows
              if zone in r.zones and r.elapsed_s >= from_elapsed_s]
    if not duties:
        return None
    return min(duties), max(duties)


def settle_criterion_audit(rows: Sequence[log_analysis.PollRow], source: str = ""
                            ) -> list:
    """For every dwell window and zone in a SINGLE run, report whether the
    firmware-matching settle test (``_zone_settle_row``) fired, and if so,
    how much that zone's duty still ranged over the rest of the SAME
    window -- the audit this module's docstring section above exists for.
    Includes zones that never settled too (``settled=False``), so a
    caller can see the full picture, not just the flagged ones.
    """
    entries = []
    for w in log_analysis.build_windows(rows):
        if w.phase != "dwell":
            continue
        window_rows = rows[w.start_idx:w.end_idx + 1]
        target_c = window_rows[-1].target_c
        for zone in ZONES:
            settle_row = _zone_settle_row(window_rows, zone)
            from_s = settle_row.elapsed_s if settle_row is not None else window_rows[0].elapsed_s
            duty_range = dwell_window_duty_range(window_rows, zone, from_elapsed_s=from_s)
            dmin, dmax = duty_range if duty_range is not None else (None, None)
            drange = (dmax - dmin) if duty_range is not None else None

            if settle_row is None:
                entries.append(SettleAuditEntry(
                    zone=zone, dwell_target_c=target_c, window_duration_s=w.duration_s,
                    settled=False, settle_elapsed_s=None, settle_duty=None,
                    window_duty_min=dmin, window_duty_max=dmax, window_duty_range=drange,
                    flagged_unstable=False, source=source,
                ))
                continue

            settle_duty = settle_row.zones[zone].duty
            flagged = False
            if drange is not None:
                if drange > UNSTABLE_DUTY_RANGE_ABS:
                    flagged = True
                if settle_duty > 0 and drange > UNSTABLE_DUTY_RANGE_FRAC * settle_duty:
                    flagged = True
            entries.append(SettleAuditEntry(
                zone=zone, dwell_target_c=target_c, window_duration_s=w.duration_s,
                settled=True, settle_elapsed_s=settle_row.elapsed_s, settle_duty=settle_duty,
                window_duty_min=dmin, window_duty_max=dmax, window_duty_range=drange,
                flagged_unstable=flagged, source=source,
            ))
    return entries


def settle_criterion_audit_from_paths(paths):
    """Same file/run handling as ``dwell_observations_from_paths``."""
    all_entries = []
    for path in paths:
        rows_all = log_analysis.parse_profile_exec_jsonl(path)
        for run_idx, rows in enumerate(log_analysis.split_runs(rows_all)):
            if not any(r.zones for r in rows):
                continue
            label = path if run_idx == 0 else f"{path}#run{run_idx}"
            all_entries.extend(settle_criterion_audit(rows, source=label))
    return all_entries


def format_settle_audit_text(entries):
    settled = [e for e in entries if e.settled]
    flagged = [e for e in settled if e.flagged_unstable]
    nl = chr(10)
    lines = [f"settle-criterion audit: {len(entries)} (window, zone) entries, "
             f"{len(settled)} settled, {len(flagged)} FLAGGED as possibly not steady state"]
    for e in entries:
        if not e.settled:
            lines.append(f"  target={e.dwell_target_c:6.1f}C zone{e.zone} dur={e.window_duration_s:5.0f}s "
                         f"NEVER SETTLED  [{e.source}]")
            continue
        marker = "FLAGGED" if e.flagged_unstable else "ok"
        lines.append(
            f"  target={e.dwell_target_c:6.1f}C zone{e.zone} dur={e.window_duration_s:5.0f}s "
            f"settled@{e.settle_elapsed_s:5.0f}s duty={e.settle_duty:.3f}  "
            f"window duty range=[{e.window_duty_min:.3f},{e.window_duty_max:.3f}] "
            f"(span {e.window_duty_range:.3f})  {marker}  [{e.source}]"
        )
    if flagged:
        lines.append("")
        lines.append(f"*** {len(flagged)}/{len(settled)} settled readings are FLAGGED: the firmware's "
                     f"settle criterion (slope on actual_c only, no duty check) fired while duty was "
                     f"still swinging by more than {UNSTABLE_DUTY_RANGE_ABS} abs or "
                     f"{UNSTABLE_DUTY_RANGE_FRAC:.0%} of the settled value over the rest of the same "
                     f"dwell -- treat these as NOT DC-gain measurements. ***")
    return nl.join(lines)


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

    Also safe to point at a RESUMED capture split across more than one
    file (e.g. ``coupid6_run1.jsonl`` + ``coupid6_run1_part2.jsonl`` after
    the board's HTTP server wedged mid-firing and a second poller was
    started against the still-running board): each path is parsed and
    settle-extracted independently, then ``_dedupe_observations`` below
    collapses any observation the two files both happened to capture
    (their elapsed_s clocks are the board's own and are NOT guaranteed to
    line up file-to-file, so this dedupes on the physical READING, not on
    elapsed_s or source).
    """
    all_obs: list[JointObservation] = []
    for path in paths:
        rows_all = log_analysis.parse_profile_exec_jsonl(path)
        for run_idx, rows in enumerate(log_analysis.split_runs(rows_all)):
            if not any(r.zones for r in rows):
                continue
            label = path if run_idx == 0 else f"{path}#run{run_idx}"
            all_obs.extend(dwell_observations_for_run(rows, source=label))
    return _dedupe_observations(all_obs)


def _dedupe_observations(observations: Sequence[JointObservation]) -> list[JointObservation]:
    """Collapse observations that are almost certainly the SAME physical
    dwell reading, captured twice (typically two overlapping poll files
    from a resumed capture -- see ``dwell_observations_from_paths``).
    Two observations are the same reading if their temperature AND duty
    vectors match to within the thermocouple/duty quantization (0.15 C,
    0.02 duty) -- tight enough that two genuinely different steady states
    essentially never collide, loose enough to survive the two pollers'
    independent rounding. First occurrence wins.
    """
    kept: list[JointObservation] = []
    for o in observations:
        is_dup = False
        for k in kept:
            if (np.max(np.abs(o.T - k.T)) <= 0.15
                    and np.max(np.abs(o.u - k.u)) <= 0.02
                    and abs(o.dwell_target_c - k.dwell_target_c) <= 0.5):
                is_dup = True
                break
        if not is_dup:
            kept.append(o)
    return kept


def parse_manual_dwell_tsv(path: str) -> list[JointObservation]:
    """Parse a hand-harvested dwell TSV like
    ``logs/coupling/coupid6_dwell_observations.tsv`` -- captured over the
    UART link when the board's HTTP poller was unavailable (server wedge),
    so it has no ``/api/profile_exec`` JSON to reuse ``parse_profile_exec_
    jsonl`` on.

    Format: comment lines starting with ``#`` (one of which must read
    ``# Ambient at run start: A0 / A1 / A2 C (zones 0/1/2).`` -- the run's
    per-zone ambient reference, same convention as ``_run_ambient``), then
    whitespace/tab-separated data rows: ``dwell_target_c t_into_dwell_s
    actual0 actual1 actual2 duty0 duty1 duty2``.

    Rows sharing the same ``dwell_target_c`` are grouped; only the LAST
    row for each target (closest to steady state) becomes a
    ``JointObservation``. Its ``settled_confidence`` is ``'confirmed'``
    only if that target has >= 2 rows (so a slope can be checked between
    the last two) AND the last row's ``t_into_dwell_s`` is past
    ``SETTLE_MIN_S`` AND that slope is within ``SETTLE_SLOPE_FLOOR_C_PER_S``
    for every zone -- otherwise ``'unconfirmed'``. A single-sample target
    (like the 46 C dwell in the coupid6 TSV, taken mid-dwell before a
    second reading) has no slope evidence at all and always comes back
    unconfirmed, regardless of how large ``t_into_dwell_s`` happens to be
    -- clearing the time floor is necessary but not sufficient for
    "settled", and this parser will not claim more confidence than the
    data supports.
    """
    ambient_vec: Optional[np.ndarray] = None
    by_target: dict = {}
    ambient_re = None
    import re
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if ambient_re is None:
                    ambient_re = re.compile(
                        r"Ambient at run start:\s*([\d.]+)\s*/\s*([\d.]+)\s*/\s*([\d.]+)")
                m = ambient_re.search(line)
                if m:
                    ambient_vec = np.array([float(m.group(i)) for i in (1, 2, 3)])
                continue
            parts = line.split()
            if len(parts) != 8:
                continue
            try:
                target_c, t_s, a0, a1, a2, d0, d1, d2 = (float(x) for x in parts)
            except ValueError:
                continue
            by_target.setdefault(target_c, []).append(
                (t_s, np.array([a0, a1, a2]), np.array([d0, d1, d2])))

    if ambient_vec is None:
        raise ValueError(f"{path}: no '# Ambient at run start: ...' comment line found")

    obs: list[JointObservation] = []
    for target_c, samples in by_target.items():
        samples.sort(key=lambda x: x[0])
        t_last, T_last, u_last = samples[-1]
        confidence = "unconfirmed"
        if len(samples) >= 2 and t_last >= SETTLE_MIN_S:
            t_prev, T_prev, _ = samples[-2]
            slope = np.max(np.abs(T_last - T_prev)) / max(t_last - t_prev, 1e-6)
            if slope <= SETTLE_SLOPE_FLOOR_C_PER_S:
                confidence = "confirmed"
        obs.append(JointObservation(
            T=T_last, ambient=ambient_vec, u=u_last, settled_zone=-1,
            dwell_target_c=target_c, source=path, elapsed_s=t_last,
            settled_confidence=confidence,
        ))
    obs.sort(key=lambda o: o.dwell_target_c)
    return obs


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


def single_zone_column_observations_from_pair(mcp_path: str, thermo_path: str, active_zone: int,
                                                max_skew_s: float = coupling_pair_log.DEFAULT_MAX_SKEW_S,
                                                ) -> list[ColumnObservation]:
    """Same extraction as ``single_zone_column_observations``, sourced from
    the two-poller ``<name>_mcp.jsonl`` / ``<name>_thermo.jsonl`` capture
    format (``coupling_pair_log.load_pair_run``) instead of a single
    ``/api/profile_exec`` JSON capture. This is the live single-zone
    excitation runs' native format -- see that module's docstring for the
    join rule and why an unmatched exec-status row is dropped whole rather
    than emitted with missing peer zones.
    """
    rows = coupling_pair_log.load_pair_run(mcp_path, thermo_path, max_skew_s=max_skew_s)
    if not rows:
        return []
    label = f"{mcp_path}+{thermo_path}"
    return single_zone_column_observations(rows, active_zone, source=label)


def settle_criterion_audit_from_pair(mcp_path: str, thermo_path: str,
                                      max_skew_s: float = coupling_pair_log.DEFAULT_MAX_SKEW_S,
                                      ) -> list:
    """``settle_criterion_audit`` sourced from a two-poller capture pair --
    see ``single_zone_column_observations_from_pair``."""
    rows = coupling_pair_log.load_pair_run(mcp_path, thermo_path, max_skew_s=max_skew_s)
    if not rows:
        return []
    label = f"{mcp_path}+{thermo_path}"
    return settle_criterion_audit(rows, source=label)


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
    p_sz.add_argument("--zone0-pair", nargs=2, metavar=("MCP_JSONL", "THERMO_JSONL"), action="append",
                       default=[], help="mcp+thermo two-poller capture pair with zone 0 driven alone "
                                          "(repeatable)")
    p_sz.add_argument("--zone1-pair", nargs=2, metavar=("MCP_JSONL", "THERMO_JSONL"), action="append",
                       default=[], help="same as --zone0-pair, zone 1 driven alone")
    p_sz.add_argument("--zone2-pair", nargs=2, metavar=("MCP_JSONL", "THERMO_JSONL"), action="append",
                       default=[], help="same as --zone0-pair, zone 2 driven alone")
    p_sz.add_argument("--score-against", nargs="*", default=[],
                       help="ordinary poll captures to score the assembled matrix against (joint dwell observations)")
    p_sz.add_argument("--json", action="store_true")

    p_audit = sub.add_parser("settle-audit", help="check whether the firmware's dwell-settle criterion "
                                                     "accepted readings that were still drifting/oscillating")
    p_audit.add_argument("jsonl_paths", nargs="+")
    p_audit.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "report":
        report = render_report(args.jsonl_paths)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            print(format_report_text(report))
    elif args.cmd == "single-zone":
        by_zone = {0: args.zone0, 1: args.zone1, 2: args.zone2}
        by_zone_pairs = {0: args.zone0_pair, 1: args.zone1_pair, 2: args.zone2_pair}
        column_obs = {}
        for z in ZONES:
            obs = []
            if by_zone.get(z):
                obs.extend(single_zone_column_observations_from_paths(by_zone[z], z))
            for mcp_path, thermo_path in by_zone_pairs.get(z, []):
                obs.extend(single_zone_column_observations_from_pair(mcp_path, thermo_path, z))
            if obs:
                column_obs[z] = obs
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
    elif args.cmd == "settle-audit":
        entries = settle_criterion_audit_from_paths(args.jsonl_paths)
        if args.json:
            print(json.dumps([dataclasses.asdict(e) for e in entries], indent=2))
        else:
            print(format_settle_audit_text(entries))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
