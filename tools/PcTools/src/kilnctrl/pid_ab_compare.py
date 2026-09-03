"""A/B comparison of two firing captures for the fuzzy-layer question in
firmware/KilnFW/docs/PID_EXPANSION_PLAN.md §3.6/§3.7: does turning the fuzzy
layer on (``strength_pct=50``) measurably help profile-7 tracking versus the
same build with it off (``strength_pct=0``)?

Reuses ``log_analysis.py``'s windowing/IAE machinery unmodified -- this
module only (a) accepts an HTTP-capture path via ``http_capture_log.py``'s
parser and (b) assembles the per-zone metric set PID_EXPANSION_PLAN.md's
§2 table already reports, so numbers here are comparable to that table:

  * whole-run and per-segment normalized IAE (``log_analysis``'s own
    definition -- time-weighted mean |error|, °C -- the same one every §2/§3
    number in the plan was generated with; see log_analysis.py's module
    docstring for why that is deliberately simpler than the firmware's own
    ``iae_normalized`` and used for windowed stats throughout this package).
  * ramp tracking error, mean and worst, per segment (worst = the larger of
    that segment's own max overshoot/undershoot magnitude).
  * dwell-entry overshoot, peak and time-to-peak after the ramp/dwell
    boundary, per segment (``log_analysis.ramp_to_dwell_transitions``).
  * dwell steady-state offset per segment (the dwell window's own
    time-weighted mean error -- by the time a dwell window is being
    computed the ramp-entry transient has already been captured separately
    by the overshoot-peak metric above, so the window mean is a reasonable
    "settled" figure without needing a second, narrower sub-window).
  * settle time per segment (``log_analysis.ramp_to_dwell_transitions``
    again).

THE HONESTY REQUIREMENT (§3.3's "Iterative tuning" item, §3.7). This kiln's
run-to-run noise floor has never been measured. The one candidate pair in
the fixtures that could have supplied it (``holdfix_clean`` vs ``final``)
is itself confounded by a 4.8 °C start-temperature difference and produced
+22.5/+47.5/+28.6 % swings -- so it is evidence that a several-degree start
delta CAN produce a large swing, not a measurement of the noise floor
itself (the swing it produced is not attributable to noise alone; the
confound is uncontrolled for in that pair too). There is no reliable way to
turn that one confounded data point into a per-degree sensitivity number
without pretending to know something we don't.

So this module does the only honest thing available: it reports the
starting-temperature delta between the two runs, per zone, and REFUSES to
declare a winner for any zone whose start-temp delta exceeds
``CONFOUND_THRESHOLD_C`` (default 1.0 °C -- deliberately tight, since the
only data point we have shows a 4.8 °C delta is already large enough to
dominate). Below that threshold it still never claims a confirmed win --
every verdict is printed as PROVISIONAL with an explicit "noise floor:
UNKNOWN" caveat, because a small start-temp delta rules out ONE known
confounder, not every possible source of run-to-run variance.

2026-09-02f -- THE START-TEMPERATURE CONFOUND IS NOT HYPOTHETICAL. Measured
directly: the "rested" precondition is relative to the thermocouple cold
junction, which lives on the board and self-heats -- it certifies "cooled to
wherever the board is now", not "cooled to ambient". Across three captures
claimed to be the same repeated configuration, start temperatures were
27.60/28.64/28.78 °C -- a 1.18 °C range against the 1.00 °C
``CONFOUND_THRESHOLD_C`` this module already refuses on; across the wider
set of captures on hand the spread reaches 3.9 °C. The owner's decision:
keep firing the same protocol (no change to the rested precondition) and
instead make the comparison machinery ACCOUNT for the confound rather than
pretend it isn't there. Two additions, both deliberately conservative:

  * :func:`fit_start_temp_sensitivity` -- an OLS regression of
    ``iae_normalized_whole_c`` (the one metric this rig's own noise-floor
    measurement shows has a comparatively tight, trustworthy floor -- see
    the module-level note below) on start temperature, fit ONLY across a set
    of captures the caller asserts are genuinely the same controller
    configuration (typically the noise-floor artifact's own
    ``generated_from`` repeat set, where by construction nothing but start
    temperature and run-to-run noise should differ). With as few as 3-6
    points this is not a precise instrument -- it is reported as a rough
    magnitude, with its own ``n`` and Pearson ``r`` printed everywhere it is
    used, and it refuses to fit at all below ``MIN_N_FOR_SENSITIVITY`` (3)
    points. It is used ONLY to show the reader how much of a raw A/B delta
    the known start-temp difference could explain (raw delta, predicted
    contribution from the fitted sensitivity, and the residual) -- it is
    reported ALONGSIDE the existing REFUSED/INDISTINGUISHABLE/PROVISIONAL
    verdict and never overrides it. An adjustment that could flip a REFUSED
    or INDISTINGUISHABLE comparison into a confident one is exactly the
    failure mode this module exists to prevent, so the verdict is computed
    from the RAW delta and the measured floor only, same as before; the
    fitted sensitivity is supplementary context, not a gate.
  * :func:`summarize_metric_floor_reliability` -- some metrics' measured
    floors are internally consistent across zones/segments (tight), others
    are not (the same metric's floor varies several-fold from one zone or
    segment to the next, meaning the "floor" itself is noisy and a
    DISTINGUISHABLE/INDISTINGUISHABLE call built on it should be read with
    that in mind). Computed directly from the noise-floor artifact's own
    entries -- e.g. this rig's measured floor for
    ``dwell_steady_state_offset_c`` ranges 0.044-0.550 °C across
    segments/zones (12x) while ``iae_normalized_whole_c`` ranges roughly
    0.029-0.134 °C (4.6x) -- tighter, though still not perfectly flat. This
    is reported alongside each comparison and in the artifact-level summary
    so a reader can see which metrics can currently support a conclusion.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
from typing import Optional, Sequence

from kilnctrl import log_analysis as la
from kilnctrl import http_capture_log as hc
from kilnctrl import noise_floor as nf

CONFOUND_THRESHOLD_C = 1.0

NOISE_FLOOR_NOTE = (
    "NOISE FLOOR: UNKNOWN. Never measured on this rig (PID_EXPANSION_PLAN.md "
    "SS3.3 'Iterative tuning', SS3.7). The only candidate data point "
    "(holdfix_clean vs final) is itself confounded by a 4.8C start-temperature "
    "delta and produced +22.5/+47.5/+28.6% swings -- evidence that start-temp "
    "alone can dominate, not a calibrated noise floor. Nothing below is a "
    "confirmed result."
)

NOISE_FLOOR_KNOWN_NOTE = (
    "NOISE FLOOR: measured (tools/PcTools/config_presets/noise_floor.json, see "
    "noise_floor.py / PID_EXPANSION_PLAN.md SS3.3). A per-(zone, metric, "
    "segment) difference smaller than its measured floor is reported "
    "INDISTINGUISHABLE below, not PROVISIONAL -- it is not attributable to "
    "whatever this A/B comparison is testing. A key with no floor entry "
    "(never covered by the repeat campaign) still falls back to PROVISIONAL."
)

#: Minimum number of (start_temp_c, iae_normalized_whole_c) points required
#: before ``fit_start_temp_sensitivity`` will fit a regression at all. 3 is
#: already an honest floor, not a good one -- a 3-point OLS fit has one
#: residual degree of freedom, so its slope is reported everywhere alongside
#: its own n and Pearson r rather than presented as a settled number.
MIN_N_FOR_SENSITIVITY = 3

#: The one metric ``fit_start_temp_sensitivity`` fits. Chosen because this
#: rig's own noise-floor measurement shows it has a comparatively tight,
#: internally-consistent floor across zones/segments (see
#: ``summarize_metric_floor_reliability``); every other metric in
#: ``noise_floor.METRIC_NAMES`` (dwell_steady_state_offset_c most of all)
#: has a floor noisy enough that fitting a "sensitivity" to it would mostly
#: be fitting noise.
SENSITIVITY_METRIC = "iae_normalized_whole_c"

#: A metric whose measured floor varies by more than this ratio (max/min,
#: across every (zone, segment) entry for that metric in the artifact) is
#: flagged as having an UNSTABLE floor -- the DISTINGUISHABLE/
#: INDISTINGUISHABLE call for that metric is only as trustworthy as the
#: floor it's gated on. Chosen as "several-fold" rather than tuned to
#: tonight's exact numbers, but deliberately set to fall between this rig's
#: two real measured clusters: ~4.4-4.6x for iae_normalized_whole_c /
#: ramp_mean_error_c (comparatively tight) and 8-30x for every other metric
#: (dwell_steady_state_offset_c, dwell_entry_*, settle_time_s,
#: ramp_worst_error_c -- unstable). With only 3 same-config repeats on hand
#: today these ratios are themselves not precise; re-check this constant
#: once a real N>=5 campaign lands (PID_EXPANSION_PLAN.md SS3.3).
FLOOR_RELIABILITY_RATIO = 5.0


# ---------------------------------------------------------------------------
# Per-run, per-zone metrics
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ZoneRunMetrics:
    zone: int
    start_temp_c: float
    iae_normalized_whole_c: float
    iae_normalized_by_segment: dict  # segment_index -> float
    ramp_mean_error_c: dict          # segment_index -> float
    ramp_worst_error_c: dict         # segment_index -> float
    dwell_entry_overshoot_peak_c: dict     # segment_index -> Optional[float]
    dwell_entry_time_to_peak_s: dict       # segment_index -> Optional[float]
    dwell_steady_state_offset_c: dict      # segment_index -> Optional[float]
    settle_time_s: dict                    # segment_index -> Optional[float]


def _segment_normalized_iae(rows: Sequence[la.PollRow], windows: Sequence[la.Window], zone: int) -> dict:
    """Combine each segment's ramp + dwell windows into one normalized IAE,
    matching how PID_EXPANSION_PLAN.md's per-segment rows (e.g. "dwell-entry
    overshoot, seg0") are keyed by segment, not by ramp/dwell separately."""
    agg: dict = {}
    for w in windows:
        s = la.window_zone_stats(w, rows, zone)
        if s is None:
            continue
        entry = agg.setdefault(w.segment_index, {"iae_raw": 0.0, "duration": 0.0})
        entry["iae_raw"] += s.iae_raw_c_s
        entry["duration"] += w.duration_s
    return {
        seg: (v["iae_raw"] / v["duration"] if v["duration"] > 0 else float("nan"))
        for seg, v in agg.items()
    }


def compute_zone_metrics(
    rows: Sequence[la.PollRow], zone: int, start_temp_c: float, band_c: float = 1.0,
) -> Optional[ZoneRunMetrics]:
    windows = la.build_windows(rows)
    if not windows:
        return None

    whole = la.Window(-1, "all", 0, len(rows) - 1, rows[0].elapsed_s, rows[-1].elapsed_s)
    whole_stats = la.window_zone_stats(whole, rows, zone)
    if whole_stats is None:
        return None

    iae_by_segment = _segment_normalized_iae(rows, windows, zone)

    ramp_mean: dict = {}
    ramp_worst: dict = {}
    dwell_offset: dict = {}
    for w in windows:
        s = la.window_zone_stats(w, rows, zone)
        if s is None:
            continue
        if w.phase == "ramp":
            ramp_mean[w.segment_index] = s.mean_error_c
            ramp_worst[w.segment_index] = max(s.max_overshoot_c, s.max_undershoot_c)
        elif w.phase == "dwell":
            dwell_offset[w.segment_index] = s.mean_error_c

    transitions = la.ramp_to_dwell_transitions(rows, windows, zone, band_c=band_c)
    overshoot_peak: dict = {}
    time_to_peak: dict = {}
    settle_time: dict = {}
    for ev in transitions:
        overshoot_peak[ev.segment_index] = ev.peak_overshoot_c
        time_to_peak[ev.segment_index] = (
            (ev.peak_overshoot_at_s - ev.transition_at_s) if ev.peak_overshoot_at_s is not None else None
        )
        settle_time[ev.segment_index] = ev.settle_time_s

    return ZoneRunMetrics(
        zone=zone,
        start_temp_c=start_temp_c,
        iae_normalized_whole_c=whole_stats.iae_normalized_c,
        iae_normalized_by_segment=iae_by_segment,
        ramp_mean_error_c=ramp_mean,
        ramp_worst_error_c=ramp_worst,
        dwell_entry_overshoot_peak_c=overshoot_peak,
        dwell_entry_time_to_peak_s=time_to_peak,
        dwell_steady_state_offset_c=dwell_offset,
        settle_time_s=settle_time,
    )


def _first_valid_start_temp(rows: Sequence[la.PollRow], zone: int) -> float:
    """The zone's starting temperature: the first row's ``actual_c`` UNLESS
    that row is invalid, in which case the first genuinely valid row's value
    is used instead.

    2026-09-02f: real HTTP captures (e.g. the noise-floor repeat set) often
    have ``actual_valid=false`` on row 0 -- the poll landed before the first
    thermocouple read completed. ``log_analysis.PollRow``'s own docstring
    documents the firmware's convention for this: ``actual = z->actual_valid
    ? z->actual_c : 0.0f`` -- an invalid reading is carried as a literal
    ``0.0`` placeholder, not a real temperature. Blindly taking ``rows[0]``
    (the previous behaviour) silently treated that placeholder as "the kiln
    started at 0.0C", which is impossible and, worse, made every affected
    run look identical to every other affected run on the confound gate --
    exactly the kind of undetected confound this module exists to catch.
    ``math.nan`` is returned (matching this module's existing "no start
    temp available" convention) if every row for this zone is invalid or the
    zone never appears."""
    for r in rows:
        s = r.zones.get(zone)
        if s is None:
            continue
        if s.actual_c != 0.0 and not math.isnan(s.actual_c):
            return s.actual_c
    return math.nan


def compute_run_metrics(rows: Sequence[la.PollRow], band_c: float = 1.0) -> dict:
    """All zones' metrics for a single (already-selected) run."""
    zones = la.zones_in_rows(rows)
    starts = {z: _first_valid_start_temp(rows, z) for z in zones}
    out = {}
    for z in zones:
        m = compute_zone_metrics(rows, z, starts.get(z, math.nan), band_c=band_c)
        if m is not None:
            out[z] = m
    return out


# ---------------------------------------------------------------------------
# Loading either capture shape, and picking the run
# ---------------------------------------------------------------------------

def load_run(path: str, run_index: Optional[int] = None) -> list:
    """Load an HTTP-capture JSONL and return ONE run's rows. HTTP captures
    are the only source this module accepts (that is the shape the fuzzy A/B
    runs are being recorded in); the classic ``HH:MM:SS {...}`` poll-capture
    shape stays log_analysis.py's own ``compare`` command's job.

    2026-09-02: this used to always take the file's most recent run, no
    questions asked. That is exactly how a telemetry poller left running
    across a kiln cooldown produced a two-run
    ``p7_oldmatrix_http.jsonl``, and an A/B compare silently took the second
    run from BOTH sides -- comparing it against itself. The output looked
    entirely plausible (matched start temps, near-identical metrics, tidy
    verdicts) and would have been believed. Now: if the file holds more than
    one run that actually carries zone data, this raises
    ``log_analysis.MultiRunError`` (naming how many runs were found and their
    start times/temps) unless ``run_index`` says which one to use. See
    ``split()`` below for splitting a multi-run capture into one file per
    run first.
    """
    all_rows = hc.poll_rows(path)
    if not all_rows:
        return []
    rows, _n_runs, _used = la.select_run(all_rows, path, run_index=run_index)
    return rows


# ---------------------------------------------------------------------------
# A/B comparison + the honesty gate
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class MetricComparison:
    zone: int
    metric: str
    segment: Optional[int]
    a: Optional[float]
    b: Optional[float]
    delta: Optional[float]  # b - a
    verdict: str  # "REFUSED: ..." or "PROVISIONAL: ..." or "n/a"
    floor_c: Optional[float] = None
    # True: |delta| >= floor_c (DISTINGUISHABLE). False: |delta| < floor_c
    # (INDISTINGUISHABLE). None: floor unknown, distinguishability cannot be
    # assessed -- NOT the same thing as "distinguishable is false".
    distinguishable: Optional[bool] = None
    # Only populated for SENSITIVITY_METRIC when a start-temp sensitivity fit
    # was available (see fit_start_temp_sensitivity / compare_runs). Never
    # changes verdict/distinguishable -- purely explanatory.
    start_temp_adjustment: Optional[dict] = None


def _cmp(zone: int, metric: str, segment: Optional[int], a: Optional[float], b: Optional[float],
         start_delta_c: float, smaller_is_better: bool = True,
         floor_c: Optional[float] = None) -> MetricComparison:
    if a is None or b is None or (isinstance(a, float) and math.isnan(a)) or (isinstance(b, float) and math.isnan(b)):
        return MetricComparison(zone, metric, segment, a, b, None, "n/a: missing data", floor_c=floor_c)
    delta = b - a
    if start_delta_c > CONFOUND_THRESHOLD_C:
        return MetricComparison(
            zone, metric, segment, a, b, delta,
            f"REFUSED: zone {zone} start-temp delta {start_delta_c:.1f}C exceeds the "
            f"{CONFOUND_THRESHOLD_C:.1f}C confound threshold -- cannot attribute this "
            f"difference to fuzzy strength",
            floor_c=floor_c, distinguishable=None,
        )
    if floor_c is not None and abs(delta) < floor_c:
        return MetricComparison(
            zone, metric, segment, a, b, delta,
            f"INDISTINGUISHABLE: |delta|={abs(delta):.3f} is below the measured noise "
            f"floor {floor_c:.3f} for this (zone, metric, segment) -- not attributable "
            f"to whatever is being compared",
            floor_c=floor_c, distinguishable=False,
        )
    if smaller_is_better:
        better = "B" if abs(b) < abs(a) else ("A" if abs(a) < abs(b) else "tie")
    else:
        better = "B" if b < a else ("A" if a < b else "tie")
    if floor_c is not None:
        floor_note = f"DISTINGUISHABLE: |delta|={abs(delta):.3f} exceeds measured floor {floor_c:.3f}"
        distinguishable = True
    else:
        floor_note = "noise floor unknown -- distinguishability cannot be assessed"
        distinguishable = None
    return MetricComparison(
        zone, metric, segment, a, b, delta,
        f"PROVISIONAL: {better} {'lower-magnitude' if smaller_is_better else 'lower'} "
        f"({floor_note} -- not a confirmed result)",
        floor_c=floor_c, distinguishable=distinguishable,
    )


# ---------------------------------------------------------------------------
# Start-temperature covariate: sensitivity fit and reporting
#
# See the module docstring's 2026-09-02f section for the honesty framing.
# This section never gates REFUSED/INDISTINGUISHABLE/PROVISIONAL -- it only
# adds supplementary numbers that show how much of a raw delta the known
# start-temp difference could explain.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class StartTempSensitivity:
    zone: int
    n: int
    slope_c_per_c: Optional[float]   # d(iae_normalized_whole_c) / d(start_temp_c)
    intercept: Optional[float]
    r: Optional[float]
    note: str


def _ols(xs: Sequence[float], ys: Sequence[float]) -> Optional[tuple]:
    n = len(xs)
    if n < 2:
        return None
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    sxx = sum((x - mean_x) ** 2 for x in xs)
    if sxx == 0.0:
        return None  # every x identical -- slope undefined, not zero
    sxy = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    slope = sxy / sxx
    intercept = mean_y - slope * mean_x
    return slope, intercept


def _pearson(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    import statistics as _stats
    if len(xs) < 2 or len(xs) != len(ys):
        return None
    try:
        return _stats.correlation(xs, ys)
    except _stats.StatisticsError:
        return None


def fit_start_temp_sensitivity(
    paths: Sequence[str], band_c: float = 1.0,
    run_indices: Optional[Sequence[Optional[int]]] = None,
) -> dict:
    """Per-zone OLS slope of ``SENSITIVITY_METRIC`` (iae_normalized_whole_c)
    on start temperature, fit across ``paths``.

    ASSUMPTIONS -- stated because they matter more than the fit:
      * every path is the SAME controller configuration (same gains/preset/
        profile). This function has no way to verify that; pass a genuine
        same-config repeat set (e.g. a noise-floor artifact's own
        ``generated_from``), never an arbitrary set of captures.
      * the relationship is treated as LINEAR over the observed start-temp
        range only -- not validated, and not to be extrapolated outside that
        range.
      * n < MIN_N_FOR_SENSITIVITY (3): refuses to fit, returns a note
        explaining why, no slope. n as small as 3-6 still makes this a rough
        magnitude, not a precise instrument -- n and r are carried alongside
        the slope everywhere it's used so nothing pretends otherwise.
    """
    if run_indices is None:
        run_indices = [None] * len(paths)
    per_zone_xy: dict = {}
    for path, ridx in zip(paths, run_indices):
        try:
            rows = load_run(path, run_index=ridx)
        except (FileNotFoundError, OSError, la.MultiRunError, IndexError):
            continue
        if not rows:
            continue
        metrics = compute_run_metrics(rows, band_c=band_c)
        for z, m in metrics.items():
            if math.isnan(m.start_temp_c) or math.isnan(m.iae_normalized_whole_c):
                continue
            per_zone_xy.setdefault(z, []).append((m.start_temp_c, m.iae_normalized_whole_c))

    out: dict = {}
    for z, pts in per_zone_xy.items():
        n = len(pts)
        if n < MIN_N_FOR_SENSITIVITY:
            out[z] = StartTempSensitivity(
                z, n, None, None, None,
                f"n={n} < MIN_N_FOR_SENSITIVITY={MIN_N_FOR_SENSITIVITY} -- insufficient "
                "data to fit a start-temp sensitivity; no adjustment reported for this zone",
            )
            continue
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        fit = _ols(xs, ys)
        if fit is None:
            out[z] = StartTempSensitivity(
                z, n, None, None, None,
                "start temperatures identical across all repeats for this zone -- slope undefined",
            )
            continue
        slope, intercept = fit
        r = _pearson(xs, ys)
        out[z] = StartTempSensitivity(
            z, n, slope, intercept, r,
            f"OLS fit, n={n} points, over the observed start-temp range "
            f"{min(xs):.2f}-{max(xs):.2f}C only. n this small means the slope's own "
            "precision is not established -- read it as a rough magnitude, not a "
            "calibrated per-degree correction.",
        )
    return out


def _start_temp_adjustment(
    sensitivity: Optional[StartTempSensitivity], start_a: float, start_b: float, raw_delta: float,
) -> Optional[dict]:
    """Decompose ``raw_delta`` (b - a) into the portion the fitted start-temp
    sensitivity would predict from (start_b - start_a), and the residual --
    i.e. regress the outcome on start temperature and report the residual,
    one of the candidate methods this was scoped to consider. Returns
    ``None`` (report nothing) if no usable fit exists for this zone."""
    if sensitivity is None or sensitivity.slope_c_per_c is None:
        return None
    if math.isnan(start_a) or math.isnan(start_b):
        return None
    start_delta = start_b - start_a
    predicted = sensitivity.slope_c_per_c * start_delta
    residual = raw_delta - predicted
    return {
        "sensitivity_n": sensitivity.n,
        "sensitivity_slope_c_per_c": sensitivity.slope_c_per_c,
        "sensitivity_r": sensitivity.r,
        "sensitivity_note": sensitivity.note,
        "start_delta_c": start_delta,
        "raw_delta": raw_delta,
        "predicted_from_start_temp": predicted,
        "residual_after_adjustment": residual,
    }


def summarize_metric_floor_reliability(artifact: Optional[dict]) -> dict:
    """For each metric name present in ``artifact['entries']``, compute the
    ratio of the largest to the smallest measured ``noise_floor_c`` across
    every (zone, segment) entry for that metric. A metric whose own floor
    swings by more than ``FLOOR_RELIABILITY_RATIO`` from one zone/segment to
    the next has an UNSTABLE floor: the floor gating its
    DISTINGUISHABLE/INDISTINGUISHABLE calls is itself noisy, so those calls
    for that metric should be read with extra caution. Returns
    ``{metric: {"n_entries": int, "floor_min": float, "floor_max": float,
    "ratio": float, "reliable": bool}}``; a metric with fewer than 2 floor
    entries reports ``reliable: None`` (not enough data to judge)."""
    if not artifact or not artifact.get("entries"):
        return {}
    by_metric: dict = {}
    for entry in artifact["entries"].values():
        floor = entry.get("noise_floor_c")
        if floor is None:
            continue
        by_metric.setdefault(entry["metric"], []).append(floor)
    out = {}
    for metric, floors in by_metric.items():
        if len(floors) < 2:
            out[metric] = {
                "n_entries": len(floors), "floor_min": floors[0] if floors else None,
                "floor_max": floors[0] if floors else None, "ratio": None, "reliable": None,
            }
            continue
        lo, hi = min(floors), max(floors)
        ratio = (hi / lo) if lo > 0 else math.inf
        out[metric] = {
            "n_entries": len(floors), "floor_min": lo, "floor_max": hi, "ratio": ratio,
            "reliable": ratio <= FLOOR_RELIABILITY_RATIO,
        }
    return out


def compare_runs(
    path_a: str, path_b: str, band_c: float = 1.0,
    run_index_a: Optional[int] = None, run_index_b: Optional[int] = None,
    noise_floor_artifact: Optional[dict] = None,
    sensitivity_paths: Optional[Sequence[str]] = None,
) -> dict:
    """``sensitivity_paths``, if given, must be a genuine same-configuration
    repeat set (see ``fit_start_temp_sensitivity``'s assumptions) used to
    estimate the start-temp sensitivity reported alongside the whole-run IAE
    comparison. Defaults to ``noise_floor_artifact['generated_from']`` when
    not given explicitly and the artifact carries one -- the noise-floor
    campaign's repeat set is exactly this kind of same-config set by
    construction. Never affects REFUSED/INDISTINGUISHABLE/PROVISIONAL --
    see the module docstring's 2026-09-02f section."""
    try:
        rows_a = load_run(path_a, run_index=run_index_a)
        rows_b = load_run(path_b, run_index=run_index_b)
    except (FileNotFoundError, OSError, la.MultiRunError, IndexError) as exc:
        return {"error": str(exc)}
    if not rows_a or not rows_b:
        return {"error": "one or both HTTP captures had no parseable rows"}

    metrics_a = compute_run_metrics(rows_a, band_c=band_c)
    metrics_b = compute_run_metrics(rows_b, band_c=band_c)
    zones = sorted(set(metrics_a) & set(metrics_b))

    start_deltas = {
        z: abs(metrics_a[z].start_temp_c - metrics_b[z].start_temp_c)
        for z in zones
        if not math.isnan(metrics_a[z].start_temp_c) and not math.isnan(metrics_b[z].start_temp_c)
    }

    def _floor(z: int, metric: str, seg: Optional[int]) -> Optional[float]:
        return nf.floor_lookup(noise_floor_artifact, z, metric, seg)

    if sensitivity_paths is None and noise_floor_artifact:
        sensitivity_paths = noise_floor_artifact.get("generated_from") or None
    sensitivity: dict = {}
    if sensitivity_paths and len(sensitivity_paths) >= MIN_N_FOR_SENSITIVITY:
        try:
            sensitivity = fit_start_temp_sensitivity(sensitivity_paths, band_c=band_c)
        except Exception:
            sensitivity = {}

    comparisons: list = []
    for z in zones:
        ma, mb = metrics_a[z], metrics_b[z]
        sd = start_deltas.get(z, math.inf)  # unknown start temp -> maximally distrustful
        whole_cmp = _cmp(z, "iae_normalized_whole_c", None, ma.iae_normalized_whole_c, mb.iae_normalized_whole_c, sd, floor_c=_floor(z, "iae_normalized_whole_c", None))
        if whole_cmp.delta is not None:
            whole_cmp.start_temp_adjustment = _start_temp_adjustment(
                sensitivity.get(z), ma.start_temp_c, mb.start_temp_c, whole_cmp.delta,
            )
        comparisons.append(whole_cmp)
        segs = sorted(set(ma.iae_normalized_by_segment) & set(mb.iae_normalized_by_segment))
        for seg in segs:
            comparisons.append(_cmp(z, "iae_normalized_c", seg, ma.iae_normalized_by_segment.get(seg), mb.iae_normalized_by_segment.get(seg), sd, floor_c=_floor(z, "iae_normalized_c", seg)))
        segs = sorted(set(ma.ramp_mean_error_c) & set(mb.ramp_mean_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_mean_error_c", seg, ma.ramp_mean_error_c.get(seg), mb.ramp_mean_error_c.get(seg), sd, floor_c=_floor(z, "ramp_mean_error_c", seg)))
        segs = sorted(set(ma.ramp_worst_error_c) & set(mb.ramp_worst_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_worst_error_c", seg, ma.ramp_worst_error_c.get(seg), mb.ramp_worst_error_c.get(seg), sd, floor_c=_floor(z, "ramp_worst_error_c", seg)))
        segs = sorted(set(ma.dwell_entry_overshoot_peak_c) & set(mb.dwell_entry_overshoot_peak_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_entry_overshoot_peak_c", seg, ma.dwell_entry_overshoot_peak_c.get(seg), mb.dwell_entry_overshoot_peak_c.get(seg), sd, floor_c=_floor(z, "dwell_entry_overshoot_peak_c", seg)))
        segs = sorted(set(ma.dwell_entry_time_to_peak_s) & set(mb.dwell_entry_time_to_peak_s))
        for seg in segs:
            a_v, b_v = ma.dwell_entry_time_to_peak_s.get(seg), mb.dwell_entry_time_to_peak_s.get(seg)
            comparisons.append(_cmp(z, "dwell_entry_time_to_peak_s", seg, a_v, b_v, sd, smaller_is_better=False, floor_c=_floor(z, "dwell_entry_time_to_peak_s", seg)))
        segs = sorted(set(ma.dwell_steady_state_offset_c) & set(mb.dwell_steady_state_offset_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_steady_state_offset_c", seg, ma.dwell_steady_state_offset_c.get(seg), mb.dwell_steady_state_offset_c.get(seg), sd, floor_c=_floor(z, "dwell_steady_state_offset_c", seg)))
        segs = sorted(set(ma.settle_time_s) & set(mb.settle_time_s))
        for seg in segs:
            a_v, b_v = ma.settle_time_s.get(seg), mb.settle_time_s.get(seg)
            comparisons.append(_cmp(z, "settle_time_s", seg, a_v, b_v, sd, smaller_is_better=False, floor_c=_floor(z, "settle_time_s", seg)))

    return {
        "path_a": path_a, "path_b": path_b,
        "noise_floor_known": bool(noise_floor_artifact and noise_floor_artifact.get("entries")),
        "zones": zones,
        "start_temps_a": {z: metrics_a[z].start_temp_c for z in zones},
        "start_temps_b": {z: metrics_b[z].start_temp_c for z in zones},
        "start_temp_deltas_c": start_deltas,
        "metrics_a": metrics_a,
        "metrics_b": metrics_b,
        "comparisons": comparisons,
        "n_rows_a": len(rows_a), "n_rows_b": len(rows_b),
        "start_temp_sensitivity": sensitivity,
        "metric_floor_reliability": summarize_metric_floor_reliability(noise_floor_artifact),
    }


# ---------------------------------------------------------------------------
# Text / JSON rendering
# ---------------------------------------------------------------------------

def format_single_run_text(path: str, metrics: dict) -> str:
    lines = [f"run: {path}"]
    for z in sorted(metrics):
        m = metrics[z]
        lines.append(f"  zone {z}  start_temp={m.start_temp_c:.2f}C")
        lines.append(f"    iae_normalized whole-run: {m.iae_normalized_whole_c:.3f}C")
        for seg in sorted(m.iae_normalized_by_segment):
            lines.append(f"    seg{seg}: iae_norm={m.iae_normalized_by_segment[seg]:.3f}C")
            if seg in m.ramp_mean_error_c:
                lines.append(
                    f"      ramp: mean_err={m.ramp_mean_error_c[seg]:+.2f}C "
                    f"worst_err={m.ramp_worst_error_c[seg]:.2f}C"
                )
            if seg in m.dwell_entry_overshoot_peak_c:
                peak = m.dwell_entry_overshoot_peak_c[seg]
                ttp = m.dwell_entry_time_to_peak_s.get(seg)
                offset = m.dwell_steady_state_offset_c.get(seg)
                settle = m.settle_time_s.get(seg)
                lines.append(
                    f"      dwell-entry: overshoot_peak="
                    f"{'n/a' if peak is None else f'{peak:+.2f}C'} "
                    f"time_to_peak={'n/a' if ttp is None else f'{ttp:.0f}s'} "
                    f"steady_offset={'n/a' if offset is None else f'{offset:+.2f}C'} "
                    f"settle_time={'n/a' if settle is None else f'{settle:.0f}s'}"
                )
    return "\n".join(lines)


def format_compare_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [
        f"A/B compare: A={report['path_a']}  B={report['path_b']}",
        NOISE_FLOOR_KNOWN_NOTE if report.get("noise_floor_known") else NOISE_FLOOR_NOTE,
        "",
    ]
    for z in report["zones"]:
        d = report["start_temp_deltas_c"].get(z, float("nan"))
        lines.append(
            f"zone {z}: start_temp A={report['start_temps_a'][z]:.2f}C "
            f"B={report['start_temps_b'][z]:.2f}C delta={d:.2f}C"
            + (f"  *** EXCEEDS {CONFOUND_THRESHOLD_C:.1f}C CONFOUND THRESHOLD ***" if d > CONFOUND_THRESHOLD_C else "")
        )
    reliability = report.get("metric_floor_reliability") or {}
    if reliability:
        lines.append("metric floor reliability (per-metric spread of the measured floor across zones/segments):")
        for metric in sorted(reliability):
            r = reliability[metric]
            if r["reliable"] is None:
                lines.append(f"  {metric:30s} n_entries={r['n_entries']} (need >=2 to judge)")
            else:
                tag = "reliable" if r["reliable"] else "UNSTABLE -- treat distinguishability calls for this metric with caution"
                lines.append(
                    f"  {metric:30s} floor {r['floor_min']:.3f}-{r['floor_max']:.3f}C "
                    f"(ratio {r['ratio']:.1f}x) -> {tag}"
                )
        lines.append("")

    lines.append("")
    for c in report["comparisons"]:
        seg = f"seg{c.segment}" if c.segment is not None else "whole-run"
        a_s = "n/a" if c.a is None else f"{c.a:+.3f}"
        b_s = "n/a" if c.b is None else f"{c.b:+.3f}"
        lines.append(f"  z{c.zone} {seg:10s} {c.metric:30s} A={a_s:>8s} B={b_s:>8s}  {c.verdict}")
        adj = c.start_temp_adjustment
        if adj:
            lines.append(
                f"      start-temp sensitivity (n={adj['sensitivity_n']}, "
                f"slope={adj['sensitivity_slope_c_per_c']:+.4f}C/C, r={adj['sensitivity_r']!r}): "
                f"raw_delta={adj['raw_delta']:+.3f} predicted_from_start_delta={adj['predicted_from_start_temp']:+.3f} "
                f"residual={adj['residual_after_adjustment']:+.3f} -- residual is the part of the delta the "
                "fitted start-temp sensitivity does NOT explain; this does not change the verdict above"
            )
            lines.append(f"      ({adj['sensitivity_note']})")
    return "\n".join(lines)


def _jsonable(obj):
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return {k: _jsonable(v) for k, v in dataclasses.asdict(obj).items()}
    if isinstance(obj, dict):
        return {str(k): _jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_jsonable(v) for v in obj]
    if isinstance(obj, float) and math.isnan(obj):
        return None
    return obj


def compare_report_to_json(report: dict) -> str:
    return json.dumps(_jsonable(report), indent=2)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-pid-ab-compare",
        description="Compare two HTTP-capture firing runs (e.g. fuzzy=0 vs fuzzy=50) against the PID_EXPANSION_PLAN.md SS2 metric set.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_single = sub.add_parser("run", help="report per-zone metrics for a single HTTP capture")
    p_single.add_argument("path")
    p_single.add_argument("--band", type=float, default=1.0)
    p_single.add_argument("--run", type=int, default=None, help="explicit run index, required if the capture holds more than one run")
    p_single.add_argument("--json", action="store_true")

    p_cmp = sub.add_parser("compare", help="A/B compare two HTTP captures, with the start-temp confound gate")
    p_cmp.add_argument("path_a")
    p_cmp.add_argument("path_b")
    p_cmp.add_argument("--band", type=float, default=1.0)
    p_cmp.add_argument("--run-a", dest="run_a", type=int, default=None, help="explicit run index for path_a, required if it holds more than one run")
    p_cmp.add_argument("--run-b", dest="run_b", type=int, default=None, help="explicit run index for path_b, required if it holds more than one run")
    p_cmp.add_argument("--json", action="store_true")
    p_cmp.add_argument("--noise-floor", dest="noise_floor_path", default=nf.DEFAULT_ARTIFACT_PATH,
                        help="path to the noise_floor.json artifact (default: the checked-in one); "
                             "pass 'none' to compare without it, as if it never existed")
    p_cmp.add_argument("--sensitivity-from", dest="sensitivity_from", default=None, nargs="+",
                        help="paths to a genuine same-configuration repeat set used to fit the "
                             "start-temp sensitivity reported alongside the whole-run IAE comparison "
                             "(default: the noise-floor artifact's own generated_from list, if any)")

    p_split = sub.add_parser(
        "split",
        help="split a multi-run HTTP capture into one file per run (<outdir>/<name>_run1.jsonl, _run2.jsonl, ...)",
    )
    p_split.add_argument("path")
    p_split.add_argument("outdir")

    args = parser.parse_args(argv)

    if args.cmd == "run":
        try:
            rows = load_run(args.path, run_index=args.run)
        except (FileNotFoundError, OSError, la.MultiRunError, IndexError) as exc:
            print(f"error: {exc}")
            return 1
        if not rows:
            print(f"error: no parseable rows in {args.path}")
            return 1
        metrics = compute_run_metrics(rows, band_c=args.band)
        if args.json:
            print(json.dumps(_jsonable(metrics), indent=2))
        else:
            print(format_single_run_text(args.path, metrics))
    elif args.cmd == "compare":
        artifact = None if args.noise_floor_path.lower() == "none" else nf.load_artifact(args.noise_floor_path)
        report = compare_runs(args.path_a, args.path_b, band_c=args.band,
                               run_index_a=args.run_a, run_index_b=args.run_b,
                               noise_floor_artifact=artifact,
                               sensitivity_paths=args.sensitivity_from)
        print(compare_report_to_json(report) if args.json else format_compare_text(report))
        return 1 if "error" in report else 0
    elif args.cmd == "split":
        try:
            paths = hc.write_split_runs(args.path, args.outdir)
        except (FileNotFoundError, OSError) as exc:
            print(f"error: {exc}")
            return 1
        if not paths:
            print(f"error: no parseable rows in {args.path}")
            return 1
        for p in paths:
            print(p)
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
