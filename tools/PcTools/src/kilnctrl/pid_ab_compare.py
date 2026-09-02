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
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
from typing import Optional, Sequence

from kilnctrl import log_analysis as la
from kilnctrl import http_capture_log as hc

CONFOUND_THRESHOLD_C = 1.0

NOISE_FLOOR_NOTE = (
    "NOISE FLOOR: UNKNOWN. Never measured on this rig (PID_EXPANSION_PLAN.md "
    "SS3.3 'Iterative tuning', SS3.7). The only candidate data point "
    "(holdfix_clean vs final) is itself confounded by a 4.8C start-temperature "
    "delta and produced +22.5/+47.5/+28.6% swings -- evidence that start-temp "
    "alone can dominate, not a calibrated noise floor. Nothing below is a "
    "confirmed result."
)


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


def compute_run_metrics(rows: Sequence[la.PollRow], band_c: float = 1.0) -> dict:
    """All zones' metrics for a single (already-selected) run."""
    starts = {z: s.actual_c for z, s in rows[0].zones.items()} if rows else {}
    out = {}
    for z in la.zones_in_rows(rows):
        m = compute_zone_metrics(rows, z, starts.get(z, math.nan), band_c=band_c)
        if m is not None:
            out[z] = m
    return out


# ---------------------------------------------------------------------------
# Loading either capture shape, and picking the run
# ---------------------------------------------------------------------------

def load_last_run(path: str) -> list:
    """Load an HTTP-capture JSONL and return its most recent complete run's
    rows. HTTP captures are the only source this module accepts (that is
    the shape the fuzzy A/B runs are being recorded in); the classic
    ``HH:MM:SS {...}`` poll-capture shape stays log_analysis.py's own
    ``compare`` command's job."""
    all_rows = hc.poll_rows(path)
    if not all_rows:
        return []
    runs = la.split_runs(all_rows)
    idx = la._default_run_index(runs)
    return runs[idx]


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


def _cmp(zone: int, metric: str, segment: Optional[int], a: Optional[float], b: Optional[float],
         start_delta_c: float, smaller_is_better: bool = True) -> MetricComparison:
    if a is None or b is None or (isinstance(a, float) and math.isnan(a)) or (isinstance(b, float) and math.isnan(b)):
        return MetricComparison(zone, metric, segment, a, b, None, "n/a: missing data")
    delta = b - a
    if start_delta_c > CONFOUND_THRESHOLD_C:
        return MetricComparison(
            zone, metric, segment, a, b, delta,
            f"REFUSED: zone {zone} start-temp delta {start_delta_c:.1f}C exceeds the "
            f"{CONFOUND_THRESHOLD_C:.1f}C confound threshold -- cannot attribute this "
            f"difference to fuzzy strength",
        )
    if smaller_is_better:
        better = "B" if abs(b) < abs(a) else ("A" if abs(a) < abs(b) else "tie")
    else:
        better = "B" if b < a else ("A" if a < b else "tie")
    return MetricComparison(
        zone, metric, segment, a, b, delta,
        f"PROVISIONAL: {better} {'lower-magnitude' if smaller_is_better else 'lower'} "
        f"(noise floor unknown -- not a confirmed result)",
    )


def compare_runs(path_a: str, path_b: str, band_c: float = 1.0) -> dict:
    try:
        rows_a = load_last_run(path_a)
        rows_b = load_last_run(path_b)
    except (FileNotFoundError, OSError) as exc:
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

    comparisons: list = []
    for z in zones:
        ma, mb = metrics_a[z], metrics_b[z]
        sd = start_deltas.get(z, math.inf)  # unknown start temp -> maximally distrustful
        comparisons.append(_cmp(z, "iae_normalized_whole_c", None, ma.iae_normalized_whole_c, mb.iae_normalized_whole_c, sd))
        segs = sorted(set(ma.iae_normalized_by_segment) & set(mb.iae_normalized_by_segment))
        for seg in segs:
            comparisons.append(_cmp(z, "iae_normalized_c", seg, ma.iae_normalized_by_segment.get(seg), mb.iae_normalized_by_segment.get(seg), sd))
        segs = sorted(set(ma.ramp_mean_error_c) & set(mb.ramp_mean_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_mean_error_c", seg, ma.ramp_mean_error_c.get(seg), mb.ramp_mean_error_c.get(seg), sd))
        segs = sorted(set(ma.ramp_worst_error_c) & set(mb.ramp_worst_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_worst_error_c", seg, ma.ramp_worst_error_c.get(seg), mb.ramp_worst_error_c.get(seg), sd))
        segs = sorted(set(ma.dwell_entry_overshoot_peak_c) & set(mb.dwell_entry_overshoot_peak_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_entry_overshoot_peak_c", seg, ma.dwell_entry_overshoot_peak_c.get(seg), mb.dwell_entry_overshoot_peak_c.get(seg), sd))
        segs = sorted(set(ma.dwell_entry_time_to_peak_s) & set(mb.dwell_entry_time_to_peak_s))
        for seg in segs:
            a_v, b_v = ma.dwell_entry_time_to_peak_s.get(seg), mb.dwell_entry_time_to_peak_s.get(seg)
            comparisons.append(_cmp(z, "dwell_entry_time_to_peak_s", seg, a_v, b_v, sd, smaller_is_better=False))
        segs = sorted(set(ma.dwell_steady_state_offset_c) & set(mb.dwell_steady_state_offset_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_steady_state_offset_c", seg, ma.dwell_steady_state_offset_c.get(seg), mb.dwell_steady_state_offset_c.get(seg), sd))
        segs = sorted(set(ma.settle_time_s) & set(mb.settle_time_s))
        for seg in segs:
            a_v, b_v = ma.settle_time_s.get(seg), mb.settle_time_s.get(seg)
            comparisons.append(_cmp(z, "settle_time_s", seg, a_v, b_v, sd, smaller_is_better=False))

    return {
        "path_a": path_a, "path_b": path_b,
        "zones": zones,
        "start_temps_a": {z: metrics_a[z].start_temp_c for z in zones},
        "start_temps_b": {z: metrics_b[z].start_temp_c for z in zones},
        "start_temp_deltas_c": start_deltas,
        "metrics_a": metrics_a,
        "metrics_b": metrics_b,
        "comparisons": comparisons,
        "n_rows_a": len(rows_a), "n_rows_b": len(rows_b),
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
        NOISE_FLOOR_NOTE,
        "",
    ]
    for z in report["zones"]:
        d = report["start_temp_deltas_c"].get(z, float("nan"))
        lines.append(
            f"zone {z}: start_temp A={report['start_temps_a'][z]:.2f}C "
            f"B={report['start_temps_b'][z]:.2f}C delta={d:.2f}C"
            + (f"  *** EXCEEDS {CONFOUND_THRESHOLD_C:.1f}C CONFOUND THRESHOLD ***" if d > CONFOUND_THRESHOLD_C else "")
        )
    lines.append("")
    for c in report["comparisons"]:
        seg = f"seg{c.segment}" if c.segment is not None else "whole-run"
        a_s = "n/a" if c.a is None else f"{c.a:+.3f}"
        b_s = "n/a" if c.b is None else f"{c.b:+.3f}"
        lines.append(f"  z{c.zone} {seg:10s} {c.metric:30s} A={a_s:>8s} B={b_s:>8s}  {c.verdict}")
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
    p_single.add_argument("--json", action="store_true")

    p_cmp = sub.add_parser("compare", help="A/B compare two HTTP captures, with the start-temp confound gate")
    p_cmp.add_argument("path_a")
    p_cmp.add_argument("path_b")
    p_cmp.add_argument("--band", type=float, default=1.0)
    p_cmp.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "run":
        try:
            rows = load_last_run(args.path)
        except (FileNotFoundError, OSError) as exc:
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
        report = compare_runs(args.path_a, args.path_b, band_c=args.band)
        print(compare_report_to_json(report) if args.json else format_compare_text(report))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
