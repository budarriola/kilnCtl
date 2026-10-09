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
import pathlib
import statistics
from typing import Optional, Sequence, Tuple

from kilnctrl import pid_ab_compare as ab
from kilnctrl import http_capture_log as hc

SCHEMA_VERSION = 2
MIN_REPEATS_FOR_FLOOR = 2

#: This module lives at tools/PcTools/src/kilnctrl/noise_floor.py, four
#: directories below the repo root. DEFAULT_ARTIFACT_PATH used to be a
#: repo-root-relative string ("tools/PcTools/config_presets/noise_floor.json"),
#: which only resolved when the process's CWD happened to BE the repo root.
#: Run from anywhere else (e.g. `pytest` from tools/PcTools, or this tool
#: invoked from a different directory) it silently missed -- load_artifact_diagnostic()
#: deliberately swallows FileNotFoundError, so the miss produced no error,
#: just a quietly downgraded "NOISE FLOOR: UNKNOWN" instead of "measured".
#: Anchoring on __file__ makes the default correct regardless of CWD.
_REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_ARTIFACT_PATH = str(_REPO_ROOT / "tools" / "PcTools" / "config_presets" / "noise_floor.json")

#: schema_version 2 adds ``start_conditions`` (per-run starting temperature,
#: read from each capture's first row -- see ``extract_start_conditions``)
#: alongside the unchanged ``entries`` block from schema_version 1. Any
#: reader that only looks at ``entries`` (i.e. ``pid_ab_compare.py`` today,
#: via its own inline ``entries`` lookup) keeps working unmodified against a
#: schema_version-2 artifact; the new block is purely additive.

#: WHY THIS EXISTS (see module docstring for the campaign-level framing).
#: The "rested" precondition the harness enforces is relative to the
#: thermocouple COLD JUNCTION, which lives on the board and self-heats --
#: it certifies "cooled to wherever the board is now", not "cooled to
#: ambient". A warmer start biases the fitted gain low (documented
#: elsewhere in this project), so drift in starting temperature across
#: supposedly-identical repeats can masquerade as noise-floor signal
#: instead of the confound it actually is. A range above this threshold
#: means the repeat set is NOT like-for-like and the computed floor may be
#: inflated by start-temperature drift. Matches
#: ``pid_ab_compare.CONFOUND_THRESHOLD_C`` -- deliberately tight, since the
#: one real example on hand (3.9 C, see the four real captures under
#: ``logs/coupling/``) is already several times this and visibly not noise.
LIKE_FOR_LIKE_THRESHOLD_C = 1.0

#: The per-run outcome metric used for the start-temp-vs-outcome
#: correlation report. Reuses ``pid_ab_compare.ZoneRunMetrics``'s own
#: ``iae_normalized_whole_c`` -- the same whole-run normalized-IAE figure
#: ``compute_repeat_spread`` already reports under this name -- rather than
#: inventing a new figure; the per-run value used here is the mean of that
#: metric across zones.
CORRELATION_METRIC = "iae_normalized_whole_c"

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


#: Suffix used for cooldown sidecar captures (see module docstring / repo
#: convention: ``<run>.jsonl.cooldown.jsonl``). A sidecar is telemetry
#: recorded AFTER the run already reached ``state=done`` -- it is not a
#: repeat firing, and it will happily parse as "one run" on its own (it
#: rarely trips the multi-run boundary check, since it is its own file), so
#: nothing upstream of this module catches it. It must be refused explicitly
#: here rather than silently analysed as if it were an Nth repeat -- see
#: ``_reject_cooldown_sidecar``.
COOLDOWN_SIDECAR_SUFFIX = ".cooldown.jsonl"


def _reject_cooldown_sidecar(path: str) -> None:
    """Raise ``ValueError`` if ``path`` is a cooldown sidecar rather than a
    run capture. Cooldown sidecars are NOT runs (see module docstring and
    ``COOLDOWN_SIDECAR_SUFFIX``); every path-accepting entry point below
    calls this before touching the file so one can never be silently folded
    into a repeat set as an extra "run"."""
    if path.endswith(COOLDOWN_SIDECAR_SUFFIX):
        raise ValueError(
            f"{path}: this is a COOLDOWN SIDECAR (telemetry recorded after "
            "state=done), not a run capture -- refusing to analyse it as a "
            "repeat firing. Pass the run capture itself, not its "
            f"{COOLDOWN_SIDECAR_SUFFIX} sidecar.")


def compute_repeat_spread(paths: Sequence[str], band_c: float = 1.0,
                           run_indices: Optional[Sequence[Optional[int]]] = None) -> list:
    """Load each path in ``paths`` as ONE run (via ``pid_ab_compare.load_run``
    -- a multi-run file raises ``log_analysis.MultiRunError`` same as
    everywhere else, since silently picking a run here would be the exact
    near-miss ``select_run`` already exists to prevent), compute its per-zone
    metrics, and return a list of :class:`SpreadStat`, one per
    ``(zone, metric, segment)`` key that appeared in at least one run.

    Requires at least 2 paths -- spread from one run is not a measurement.
    Any path that is a cooldown sidecar (see ``_reject_cooldown_sidecar``)
    raises ``ValueError`` before any file is opened."""
    if len(paths) < 2:
        raise ValueError(
            f"compute_repeat_spread needs at least 2 repeat captures to measure a spread, got {len(paths)}")
    for path in paths:
        _reject_cooldown_sidecar(path)

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
# Start conditions (the start-temperature covariate)
# ---------------------------------------------------------------------------

def _finite_number(v) -> Optional[float]:
    if isinstance(v, bool):
        return None
    if not isinstance(v, (int, float)):
        return None
    v = float(v)
    return v if math.isfinite(v) else None


def _select_http_run(path: str, run_index: Optional[int] = None):
    """Load ``path`` as HTTP-capture rows and return the rows of ONE run,
    mirroring ``log_analysis.select_run``'s boundary rule (a new run starts
    wherever ``elapsed_s`` decreases) and its "most recent run with data"
    default -- but operating on ``http_capture_log.HttpPollRow`` so the raw
    ``status`` body (where the start-temperature covariate lives) survives
    alongside the parsed ``PollRow``.

    Returns ``[]`` for anything that isn't a normal single/multi-run HTTP
    capture (missing file, empty file, no parseable rows) -- callers treat
    that as "no start conditions available", never as a crash. A
    ``*.cooldown.jsonl`` sidecar is not itself split out here: it is simply
    not one of the paths a caller passes in as a run.
    """
    try:
        http_rows = hc.parse_http_capture_jsonl(path)
    except (FileNotFoundError, OSError):
        return []
    if not http_rows:
        return []

    runs = [[http_rows[0]]]
    for r in http_rows[1:]:
        if r.poll.elapsed_s < runs[-1][-1].poll.elapsed_s:
            runs.append([])
        runs[-1].append(r)

    if run_index is not None:
        if not (0 <= run_index < len(runs)):
            return []
        return runs[run_index]

    # Mirror log_analysis.select_run's ambiguity rule exactly: if MORE THAN
    # ONE run carries zone data, an unqualified default would be the same
    # kind of silent guess MultiRunError exists to prevent (see
    # ab.load_run's docstring) -- here that means "no start condition",
    # not a guess, rather than raising (this function is a best-effort
    # covariate reader, not the load path itself).
    nonempty = [i for i, r in enumerate(runs) if any(rr.poll.zones for rr in r)]
    if len(nonempty) > 1:
        return []
    for i in range(len(runs) - 1, -1, -1):
        if any(rr.poll.zones for rr in runs[i]):
            return runs[i]
    return runs[-1] if runs else []


def extract_start_conditions(path: str, run_index: Optional[int] = None) -> Optional[dict]:
    """Read the STARTING CONDITIONS (per-channel temperature, cold-junction
    temperature, and enclosure temperature) from the first row of one run in
    an HTTP capture.

    Returns ``None`` -- never raises -- for a missing file, an empty or
    unparseable capture, a run whose first row has no ``status`` or no
    ``channels``, or any other malformed shape; a channel entry that is
    itself malformed (not a dict, non-numeric ``temp_c``/``cj_c``) is
    dropped from that channel's numbers rather than aborting the whole
    extraction. ``valid`` follows the channel's own ``valid`` flag from the
    board -- an invalid channel's numbers are kept in ``channels`` for
    visibility but excluded from every summary figure (means, deltas).
    """
    chosen = _select_http_run(path, run_index=run_index)
    if not chosen:
        return None
    first = chosen[0]
    status = first.status
    if not isinstance(status, dict):
        return None
    raw_channels = status.get("channels")
    if not isinstance(raw_channels, list):
        raw_channels = []

    channels = []
    for i, ch in enumerate(raw_channels):
        if not isinstance(ch, dict):
            continue
        temp_c = _finite_number(ch.get("temp_c"))
        cj_c = _finite_number(ch.get("cj_c"))
        valid = bool(ch.get("valid", False))
        delta_c = (temp_c - cj_c) if (valid and temp_c is not None and cj_c is not None) else None
        channels.append({
            "channel": ch.get("channel", i),
            "temp_c": temp_c,
            "cj_c": cj_c,
            "valid": valid,
            "delta_c": delta_c,
        })

    enclosure_temp_c = _finite_number(status.get("enclosure_temp_c"))

    valid_temps = [c["temp_c"] for c in channels if c["valid"] and c["temp_c"] is not None]
    valid_cj = [c["cj_c"] for c in channels if c["valid"] and c["cj_c"] is not None]
    valid_deltas = [c["delta_c"] for c in channels if c["delta_c"] is not None]

    if not channels:
        return None

    return {
        "path": path,
        "channels": channels,
        "enclosure_temp_c": enclosure_temp_c,
        "start_temp_c_mean": statistics.fmean(valid_temps) if valid_temps else None,
        "cj_c_mean": statistics.fmean(valid_cj) if valid_cj else None,
        "delta_c_mean": statistics.fmean(valid_deltas) if valid_deltas else None,
    }


def _pearson_r(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    if len(xs) < 2 or len(xs) != len(ys):
        return None
    try:
        return statistics.correlation(xs, ys)
    except statistics.StatisticsError:
        # Zero variance in one series (e.g. every start temp identical) --
        # correlation is undefined, not zero.
        return None


def _per_run_outcome(path: str, band_c: float, run_index: Optional[int] = None) -> Optional[float]:
    """The correlation report's per-run outcome value: the mean, across
    zones, of ``CORRELATION_METRIC`` (``iae_normalized_whole_c``) --
    reusing ``pid_ab_compare.compute_run_metrics`` rather than inventing a
    new figure. ``None`` for anything unloadable/malformed, never a raise."""
    try:
        rows = ab.load_run(path, run_index=run_index)
    except (FileNotFoundError, OSError, ValueError):
        return None
    if not rows:
        return None
    try:
        metrics = ab.compute_run_metrics(rows, band_c=band_c)
    except Exception:
        return None
    vals = [
        m.iae_normalized_whole_c for m in metrics.values()
        if m.iae_normalized_whole_c is not None and not (
            isinstance(m.iae_normalized_whole_c, float) and math.isnan(m.iae_normalized_whole_c))
    ]
    return statistics.fmean(vals) if vals else None


def build_start_report(paths: Sequence[str], band_c: float = 1.0,
                        run_indices: Optional[Sequence[Optional[int]]] = None,
                        like_threshold_c: float = LIKE_FOR_LIKE_THRESHOLD_C) -> dict:
    """Build the start-temperature covariate report for a set of repeat
    captures: per-run starting conditions, the spread across runs, whether
    that spread makes the set like-for-like, the run-order trend (a
    monotonic trend is the signature of board self-heating drift, not
    random scatter), and the (weak, n-limited) correlation against the
    existing per-run outcome metric.

    Every field degrades gracefully: a run with no extractable start
    condition is simply omitted from the numeric summaries (and counted in
    ``n_missing``), never raises.
    """
    for path in paths:
        _reject_cooldown_sidecar(path)

    if run_indices is None:
        run_indices = [None] * len(paths)

    runs = []
    for path, run_idx in zip(paths, run_indices):
        cond = extract_start_conditions(path, run_index=run_idx)
        outcome = _per_run_outcome(path, band_c, run_index=run_idx)
        runs.append({"path": path, "start": cond, "outcome": outcome})

    starts_in_order = [r["start"]["start_temp_c_mean"] for r in runs
                        if r["start"] is not None and r["start"]["start_temp_c_mean"] is not None]
    n_missing = len(paths) - len(starts_in_order)

    if starts_in_order:
        lo, hi = min(starts_in_order), max(starts_in_order)
        rng = hi - lo
        like_for_like = rng <= like_threshold_c
    else:
        lo = hi = rng = None
        like_for_like = None

    if len(starts_in_order) >= 2:
        diffs = [b - a for a, b in zip(starts_in_order, starts_in_order[1:])]
        monotonic = all(d >= 0 for d in diffs) or all(d <= 0 for d in diffs)
    else:
        monotonic = None

    warning = None
    if like_for_like is False:
        warning = (
            f"start temperatures span {rng:.2f}C (min {lo:.2f}, max {hi:.2f}) across "
            f"{len(starts_in_order)} runs, exceeding the {like_threshold_c:.2f}C "
            "like-for-like threshold -- these runs are NOT a like-for-like repeat "
            "set, and the noise floor computed from them may be inflated by "
            "start-temperature drift (self-heating of the on-board cold junction) "
            "rather than measuring controller noise alone."
        )

    pairs = [
        {"path": r["path"], "start_temp_c": r["start"]["start_temp_c_mean"], "outcome": r["outcome"]}
        for r in runs
        if r["start"] is not None and r["start"]["start_temp_c_mean"] is not None and r["outcome"] is not None
    ]
    r_value = _pearson_r([p["start_temp_c"] for p in pairs], [p["outcome"] for p in pairs])

    return {
        "like_for_like_threshold_c": like_threshold_c,
        "n_runs": len(paths),
        "n_missing_start_temp": n_missing,
        "runs": runs,
        "start_temp_c_min": lo,
        "start_temp_c_max": hi,
        "start_temp_c_range": rng,
        "like_for_like": like_for_like,
        "monotonic_with_run_order": monotonic,
        "warning": warning,
        "vs_outcome": {
            "metric": f"{CORRELATION_METRIC} (mean across zones)",
            "n": len(pairs),
            "pairs": pairs,
            "pearson_r": r_value,
            "caution": (
                "n is too small for a correlation coefficient to be meaningful on its "
                "own at this sample size -- read the raw pairs, not just pearson_r, and "
                "do not treat this as evidence of a start-temp effect from this sample "
                "alone."
            ),
        },
    }


def format_start_report_text(report: dict) -> str:
    lines = []
    lo, hi, rng = report["start_temp_c_min"], report["start_temp_c_max"], report["start_temp_c_range"]
    if rng is None:
        lines.append("start temperature: no runs had extractable start conditions")
    else:
        lines.append(
            f"start temperature: min={lo:.2f}C max={hi:.2f}C range={rng:.2f}C "
            f"(threshold={report['like_for_like_threshold_c']:.2f}C) "
            f"-> {'LIKE-FOR-LIKE' if report['like_for_like'] else 'NOT LIKE-FOR-LIKE'}"
        )
        if report["monotonic_with_run_order"]:
            lines.append(
                "  ordering: start temperature is MONOTONIC with run index -- the "
                "signature of self-heating drift, not random scatter"
            )
        elif report["monotonic_with_run_order"] is not None:
            lines.append("  ordering: start temperature is NOT monotonic with run index")
        if report["warning"]:
            lines.append(f"  WARNING: {report['warning']}")
    if report["n_missing_start_temp"]:
        lines.append(f"  ({report['n_missing_start_temp']} of {report['n_runs']} runs had no extractable start condition)")
    vs = report["vs_outcome"]
    lines.append(f"start-temp vs {vs['metric']}: n={vs['n']} pearson_r={vs['pearson_r']!r}")
    lines.append(f"  {vs['caution']}")
    for p in vs["pairs"]:
        lines.append(f"    {p['path']}: start={p['start_temp_c']:.2f}C outcome={p['outcome']:.4f}")
    return "\n".join(lines)


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
    start_report = build_start_report(paths, band_c=band_c, run_indices=run_indices)
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
            "compared -- see pid_ab_compare.py's use of this artifact. See "
            "start_conditions for whether 'genuinely rested' actually held: the "
            "harness's rested check is relative to the on-board cold junction, not "
            "ambient, so it does not by itself guarantee like-for-like starts."
        ),
        "entries": entries,
        "start_conditions": start_report,
    }


def load_artifact_diagnostic(path: str = DEFAULT_ARTIFACT_PATH) -> Tuple[Optional[dict], Optional[str]]:
    """Load the checked-in noise-floor artifact, returning ``(artifact,
    reason)``. ``reason`` is ``None`` on success; on failure it is a short,
    human-readable string distinguishing the two ways a load can fail so a
    caller can surface *why* the floor is unavailable instead of silently
    treating every failure as "never measured":

      * "missing" -- no file at ``path`` at all (wrong path, artifact never
        generated).
      * "unreadable" -- a file exists at ``path`` but couldn't be parsed as
        the expected JSON (permission error, truncated/corrupt write).

    Never raises -- "unknown is a valid state, not a crash": callers
    (``pid_ab_compare.py``) must treat ``(None, reason)`` as "floor not
    measured", not an error."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f), None
    except FileNotFoundError:
        return None, f"missing -- no artifact file at {path}"
    except (OSError, json.JSONDecodeError) as exc:
        return None, f"unreadable -- {path} exists but failed to load ({exc})"


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
        start_report = build_start_report(args.paths, band_c=args.band)
        if args.json:
            print(json.dumps([dataclasses.asdict(s) for s in stats], indent=2))
            print(json.dumps(start_report, indent=2))
        else:
            print(format_spread_text(stats))
            print()
            print(format_start_report_text(start_report))
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
        print()
        print(format_start_report_text(artifact["start_conditions"]))
        return 0
    else:
        parser.print_help()
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
