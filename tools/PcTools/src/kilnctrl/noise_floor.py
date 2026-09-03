"""Measure and publish the run-to-run NOISE FLOOR for profile-7 firings
(PID_EXPANSION_PLAN.md §3.3's iterative-tuning item, §3.7).

WHY THIS EXISTS. ``pid_ab_compare.py`` has always reported "NOISE FLOOR:
UNKNOWN" -- every verdict it ever printed was PROVISIONAL because nobody had
measured how much a metric moves between two firings of the SAME
configuration (same gains, same coupling matrix) from a genuinely rested
start. The nearest thing on hand (``holdfix_clean`` vs ``final``) was itself
confounded by a 4.8 °C start-temp delta, so it measured "a several-degree
start delta produces a large swing", not the floor.

THIS MODULE closes that gap in two steps:

  1. :func:`compute_repeat_spread` -- given N single-run HTTP captures that
     are claimed to be true repeats of one configuration (same preset, same
     profile), compute the per-zone, per-metric spread (mean, sample std,
     range) across the N runs, using exactly the metric set
     ``pid_ab_compare.compute_run_metrics`` reports, so the numbers line up
     one-to-one with ``pid_ab_compare``'s own comparison keys
     (zone, metric, segment).
  2. :func:`build_artifact` / the ``build`` CLI subcommand turns that spread
     into a checked-in JSON artifact (default
     ``tools/PcTools/config_presets/noise_floor.json``, matching the
     ``tuning_recommendations.json`` pattern from ``tuning_campaign.py``) that
     ``pid_ab_compare.py`` loads and uses as its confound/indistinguishable
     gate instead of a hardcoded "unknown".

THE FLOOR NUMBER, per (zone, metric[, segment]), is the observed RANGE
(max - min) across the N repeats, not the standard deviation. With N as
small as this rig can realistically produce overnight (5-10 firings, each
~30-45 minutes plus a rested wait), a range is a defensible worst-case bound
where a std would understate the tail on so few samples; std is still
reported alongside it for context. A metric with fewer than
``MIN_REPEATS_FOR_FLOOR`` (2) values is reported with ``n`` but no floor --
you cannot measure spread from one sample.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import statistics
from typing import Optional, Sequence

from kilnctrl import pid_ab_compare as ab

SCHEMA_VERSION = 1
MIN_REPEATS_FOR_FLOOR = 2

DEFAULT_ARTIFACT_PATH = "tools/PcTools/config_presets/noise_floor.json"

#: Metric names that are "lower is worse" in the sense that a NEGATIVE range
#: is meaningless -- kept here only for documentation; the spread computation
#: below is symmetric (range/std of the raw signed values) for every metric,
#: matching how pid_ab_compare's own deltas are signed.
METRIC_NAMES = (
    "iae_normalized_whole_c",
    "iae_normalized_c",
    "ramp_mean_error_c",
    "ramp_worst_error_c",
    "dwell_entry_overshoot_peak_c",
    "dwell_entry_time_to_peak_s",
    "dwell_steady_state_offset_c",
    "settle_time_s",
)


@dataclasses.dataclass
class SpreadStat:
    zone: int
    metric: str
    segment: Optional[int]
    n: int
    values: list
    mean: Optional[float]
    std_c: Optional[float]
    range_c: Optional[float]


def _flat_metric_values(metrics: dict) -> dict:
    """Flatten one run's ``{zone: ZoneRunMetrics}`` into
    ``{(zone, metric, segment): value}``, using the exact metric names
    ``pid_ab_compare.compare_runs`` uses for its own comparisons -- so a
    (zone, metric, segment) key here is directly usable as a lookup key
    there."""
    out: dict = {}
    for z, m in metrics.items():
        out[(z, "iae_normalized_whole_c", None)] = m.iae_normalized_whole_c
        for seg, v in m.iae_normalized_by_segment.items():
            out[(z, "iae_normalized_c", seg)] = v
        for seg, v in m.ramp_mean_error_c.items():
            out[(z, "ramp_mean_error_c", seg)] = v
        for seg, v in m.ramp_worst_error_c.items():
            out[(z, "ramp_worst_error_c", seg)] = v
        for seg, v in m.dwell_entry_overshoot_peak_c.items():
            out[(z, "dwell_entry_overshoot_peak_c", seg)] = v
        for seg, v in m.dwell_entry_time_to_peak_s.items():
            out[(z, "dwell_entry_time_to_peak_s", seg)] = v
        for seg, v in m.dwell_steady_state_offset_c.items():
            out[(z, "dwell_steady_state_offset_c", seg)] = v
        for seg, v in m.settle_time_s.items():
            out[(z, "settle_time_s", seg)] = v
    return out


def compute_repeat_spread(paths: Sequence[str], band_c: float = 1.0,
                           run_indices: Optional[Sequence[Optional[int]]] = None) -> list:
    """Load each path in ``paths`` as ONE run (via ``pid_ab_compare.load_run``
    -- a multi-run file raises ``log_analysis.MultiRunError`` same as
    everywhere else, since silently picking a run here would be the exact
    near-miss ``select_run`` already exists to prevent), compute its per-zone
    metrics, and return a list of :class:`SpreadStat`, one per
    ``(zone, metric, segment)`` key that appeared in at least one run.

    Requires at least 2 paths -- spread from one run is not a measurement."""
    if len(paths) < 2:
        raise ValueError(
            f"compute_repeat_spread needs at least 2 repeat captures to measure a spread, got {len(paths)}")

    if run_indices is None:
        run_indices = [None] * len(paths)
    if len(run_indices) != len(paths):
        raise ValueError("run_indices must be the same length as paths when given")

    per_run_flat = []
    for path, run_idx in zip(paths, run_indices):
        rows = ab.load_run(path, run_index=run_idx)
        if not rows:
            raise ValueError(f"{path}: no parseable rows")
        metrics = ab.compute_run_metrics(rows, band_c=band_c)
        per_run_flat.append(_flat_metric_values(metrics))

    all_keys = set()
    for flat in per_run_flat:
        all_keys.update(flat.keys())

    out = []
    for (zone, metric, segment) in sorted(all_keys, key=lambda k: (k[0], k[1], -1 if k[2] is None else k[2])):
        values = []
        for flat in per_run_flat:
            v = flat.get((zone, metric, segment))
            if v is None or (isinstance(v, float) and math.isnan(v)):
                continue
            values.append(v)
        n = len(values)
        if n >= MIN_REPEATS_FOR_FLOOR:
            mean = statistics.fmean(values)
            std_c = statistics.stdev(values) if n >= 2 else 0.0
            range_c = max(values) - min(values)
        else:
            mean = values[0] if values else None
            std_c = None
            range_c = None
        out.append(SpreadStat(zone=zone, metric=metric, segment=segment, n=n,
                               values=values, mean=mean, std_c=std_c, range_c=range_c))
    return out


# ---------------------------------------------------------------------------
# Artifact
# ---------------------------------------------------------------------------

def _key_str(zone: int, metric: str, segment: Optional[int]) -> str:
    return f"z{zone}:{metric}:{'whole' if segment is None else segment}"


def build_artifact(paths: Sequence[str], band_c: float = 1.0,
                    run_indices: Optional[Sequence[Optional[int]]] = None) -> dict:
    stats = compute_repeat_spread(paths, band_c=band_c, run_indices=run_indices)
    entries = {}
    for s in stats:
        if s.range_c is None:
            continue  # fewer than MIN_REPEATS_FOR_FLOOR values -- no floor
        entries[_key_str(s.zone, s.metric, s.segment)] = {
            "zone": s.zone,
            "metric": s.metric,
            "segment": s.segment,
            "n": s.n,
            "mean": s.mean,
            "std_c": s.std_c,
            "noise_floor_c": s.range_c,
        }
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_from": list(paths),
        "band_c": band_c,
        "n_repeats": len(paths),
        "note": (
            "noise_floor_c is the observed RANGE (max-min) across the repeat captures "
            "listed in generated_from, all claimed to be the same preset+profile fired "
            "from a genuinely rested start. A difference smaller than this for a given "
            "(zone, metric, segment) key is not attributable to whatever is being A/B "
            "compared -- see pid_ab_compare.py's use of this artifact."
        ),
        "entries": entries,
    }


def load_artifact(path: str = DEFAULT_ARTIFACT_PATH) -> Optional[dict]:
    """Load the checked-in noise-floor artifact, or ``None`` if it doesn't
    exist yet -- callers (``pid_ab_compare.py``) must treat that as "unknown"
    exactly like before this module existed, never as a crash."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return None


def floor_lookup(artifact: Optional[dict], zone: int, metric: str, segment: Optional[int]) -> Optional[float]:
    """Return the measured noise floor (°C, or seconds for the two time
    metrics) for one comparison key, or ``None`` if the artifact is absent or
    has no entry for that key."""
    if not artifact:
        return None
    entry = artifact.get("entries", {}).get(_key_str(zone, metric, segment))
    if entry is None:
        return None
    return entry.get("noise_floor_c")


# ---------------------------------------------------------------------------
# Text rendering
# ---------------------------------------------------------------------------

def format_spread_text(stats: Sequence[SpreadStat]) -> str:
    lines = []
    for s in stats:
        if s.range_c is None:
            lines.append(f"  z{s.zone} {s.metric:30s} seg={s.segment}  n={s.n}  (need >=2 to measure spread)")
            continue
        lines.append(
            f"  z{s.zone} {s.metric:30s} seg={s.segment}  n={s.n}  "
            f"mean={s.mean:+.3f}  std={s.std_c:.3f}  range(FLOOR)={s.range_c:.3f}"
        )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-noise-floor",
        description="Measure the run-to-run noise floor from N repeat captures of one profile-7 configuration.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_report = sub.add_parser("report", help="print the per-zone/metric spread for N repeat captures")
    p_report.add_argument("paths", nargs="+")
    p_report.add_argument("--band", type=float, default=1.0)
    p_report.add_argument("--json", action="store_true")

    p_build = sub.add_parser("build", help="write the checked-in noise_floor.json artifact")
    p_build.add_argument("paths", nargs="+")
    p_build.add_argument("--band", type=float, default=1.0)
    p_build.add_argument("--out", default=DEFAULT_ARTIFACT_PATH)

    args = parser.parse_args(argv)

    if args.cmd == "report":
        try:
            stats = compute_repeat_spread(args.paths, band_c=args.band)
        except (FileNotFoundError, OSError, ValueError) as exc:
            print(f"error: {exc}")
            return 1
        if args.json:
            print(json.dumps([dataclasses.asdict(s) for s in stats], indent=2))
        else:
            print(format_spread_text(stats))
        return 0
    elif args.cmd == "build":
        try:
            artifact = build_artifact(args.paths, band_c=args.band)
        except (FileNotFoundError, OSError, ValueError) as exc:
            print(f"error: {exc}")
            return 1
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(artifact, f, indent=2)
            f.write("\n")
        print(f"wrote {args.out} ({len(artifact['entries'])} entries from {len(args.paths)} repeats)")
        return 0
    else:
        parser.print_help()
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
