"""Offline analysis of firing/tuning logs -- windowed statistics, autotune
cross-checks, run comparison, and sanity checks against the firmware's own
computed numbers.

Nothing here touches the board, a serial port, or firmware source. It is a
pure-Python reader over three input shapes:

  * poll-capture JSONL: one line per poll, ``HH:MM:SS {json}`` where the JSON
    is a raw ``/api/profile_exec`` or ``/api/autotune`` response body (see
    ``dashboard_http.c`` / ``autotune_engine.c`` for the field definitions).
  * the board's own CSV exports: ``/api/history.csv`` and
    ``/api/autotune/trace.csv``.
  * (future) a structured telemetry file from the debug-UART capture another
    agent is building tonight. See ``SOURCE KINDS`` below for where that
    plugs in -- nothing else in this module should need to change.

Everything downstream (windowing, stats, the FOPDT refit, comparisons) works
on the small, source-agnostic dataclasses defined here (``PollRow``,
``AutotuneRow``), not on the raw JSON, so a new source only has to produce
those.

Design choices made explicitly so they can be argued with:

  * Per-window statistics are computed from raw per-row samples (error =
    ``actual_c - target_c``, weighted by the elapsed-time delta between
    polls). The firmware's own ``firing_stats`` block is a *cumulative*
    total since the run started (its ``sample_count`` only grows), not a
    per-window figure -- that is exactly the gap this tool fills, and also
    why the two numbers are not expected to match exactly. Where a
    comparison is useful (whole-run totals) it is called out as such.
  * "Normalized IAE" here means the raw IAE (integral of |error| dt, in
    C*s) divided by the window duration in seconds -- i.e. the time-weighted
    mean absolute error, in degrees C. This is deliberately a different
    (simpler, easier to hand-verify and to compare across windows of
    different length) definition than the firmware's own
    ``iae_normalized``, which additionally divides by the segment's
    setpoint span. Both are reported for a raw autotune/firing summary;
    windowed stats use only ours.
  * Settle band defaults to +/-1.0 C. That is roughly the LCD display's own
    rounding and a defensible "close enough to call it holding" tolerance
    for a kiln; pass ``--band`` to use another.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import math
import sys
from typing import Iterable, Optional, Sequence


# ---------------------------------------------------------------------------
# SOURCE KINDS -- where a new telemetry source plugs in.
#
# Every parser in this file ends by producing a list of PollRow (firing) or
# AutotuneRow (tune) records. The structured UART telemetry stream being
# built tonight is a third source of PollRow/AutotuneRow data; once its file
# format is known, add one function here --
# ``parse_profile_exec_uart_capture(path) -> list[PollRow]`` -- that emits
# the same dataclass, and every analysis function below (windowing, stats,
# settle time, comparisons) works unmodified. Nothing about the analysis
# layer should need to know which of the three sources produced its input.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ZoneSample:
    zone: int
    actual_c: float
    duty: float
    ff_hold_used_matrix: bool = False
    ff_hold_infeasible: bool = False
    # Verbatim copy of the firmware's own cumulative-since-start stats for
    # this zone, if present -- used only for the sanity-check / "compare to
    # firmware" path, never for our own windowed numbers.
    firmware_firing_stats: Optional[dict] = None


@dataclasses.dataclass
class PollRow:
    """One line of a ``/api/profile_exec`` poll capture."""
    wall_time: str
    elapsed_s: float
    segment_index: int
    segment_count: int
    dwelling: bool
    target_c: float
    state: str
    zones: dict  # zone number -> ZoneSample


@dataclasses.dataclass
class AutotuneRow:
    """One line of an ``/api/autotune`` poll capture."""
    wall_time: str
    elapsed_s: float
    state: str
    zone: int
    method: str
    actual_c: float
    duty: float
    sample_count: int
    model_valid: bool
    model_settled: bool
    model_extrapolation_converged: bool
    model_tau_consistent: bool
    k_gain_c_per_duty: float
    tau_s: float
    dead_time_s: float
    baseline_c: float
    final_c: float
    raw_rise_c: float
    rise_inf_c: float
    step_ambient_c: float
    proposed_kp: float
    proposed_ki: float
    proposed_kd: float
    abort_reason: str


# ---------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------

def _split_capture_line(line: str) -> Optional[tuple[str, dict]]:
    line = line.strip()
    if not line:
        return None
    wall_time, _, rest = line.partition(" ")
    rest = rest.strip()
    if not rest.startswith("{"):
        return None
    try:
        return wall_time, json.loads(rest)
    except json.JSONDecodeError:
        return None


def _zone_samples_from_exec_body(body: dict) -> dict:
    """Build the ``{zone -> ZoneSample}`` dict shared by every parser that
    consumes a raw ``/api/profile_exec`` response body, however that body
    reached this process (a bare poll-capture line, or nested inside some
    other envelope like the HTTP-capture ``{"t","exec","status"}`` shape).
    """
    zones = {}
    for z in body.get("zones", []):
        zones[z["zone"]] = ZoneSample(
            zone=z["zone"],
            actual_c=z.get("actual_c", math.nan),
            duty=z.get("duty", 0.0),
            ff_hold_used_matrix=z.get("ff_hold_used_matrix", False),
            ff_hold_infeasible=z.get("ff_hold_infeasible", False),
            firmware_firing_stats=z.get("firing_stats"),
        )
    return zones


def poll_row_from_exec_body(wall_time: str, body: dict) -> PollRow:
    """Build a ``PollRow`` from one raw ``/api/profile_exec`` response body.

    Shared by every parser in this package that has such a body in hand
    (``parse_profile_exec_jsonl`` below, and ``http_capture_log.py``'s
    HTTP-capture parser) -- one place defines the field mapping so a new
    source only has to locate the body and a wall-clock label, never
    reimplement this.
    """
    return PollRow(
        wall_time=wall_time,
        elapsed_s=float(body.get("elapsed_s", 0)),
        segment_index=int(body.get("segment_index", 0)),
        segment_count=int(body.get("segment_count", 0)),
        dwelling=bool(body.get("dwelling", False)),
        target_c=float(body.get("target_c", math.nan)),
        state=body.get("state", ""),
        zones=_zone_samples_from_exec_body(body),
    )


def parse_profile_exec_jsonl(path: str) -> list[PollRow]:
    """Parse a captured ``HH:MM:SS {profile_exec body}`` log.

    Lines that are not ``profile_exec`` bodies (e.g. an autotune capture
    mixed into the same file, or a blank/malformed line) are skipped rather
    than raising -- a poll capture running for hours over a flaky link is
    expected to have the odd truncated line.
    """
    rows: list[PollRow] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parsed = _split_capture_line(line)
            if parsed is None:
                continue
            wall_time, body = parsed
            if "zones" not in body or "dwelling" not in body:
                continue
            rows.append(poll_row_from_exec_body(wall_time, body))
    return rows


def parse_autotune_jsonl(path: str) -> list[AutotuneRow]:
    """Parse a captured ``HH:MM:SS {autotune body}`` log."""
    rows: list[AutotuneRow] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parsed = _split_capture_line(line)
            if parsed is None:
                continue
            wall_time, body = parsed
            if "model_valid" not in body or "zone" not in body:
                continue
            rows.append(AutotuneRow(
                wall_time=wall_time,
                elapsed_s=float(body.get("elapsed_s", 0)),
                state=body.get("state", ""),
                zone=int(body.get("zone", 0)),
                method=body.get("method", ""),
                actual_c=float(body.get("actual_c", math.nan)),
                duty=float(body.get("duty", 0.0)),
                sample_count=int(body.get("sample_count", 0)),
                model_valid=bool(body.get("model_valid", False)),
                model_settled=bool(body.get("model_settled", False)),
                model_extrapolation_converged=bool(body.get("model_extrapolation_converged", False)),
                model_tau_consistent=bool(body.get("model_tau_consistent", False)),
                k_gain_c_per_duty=float(body.get("k_gain_c_per_duty", 0.0)),
                tau_s=float(body.get("tau_s", 0.0)),
                dead_time_s=float(body.get("dead_time_s", 0.0)),
                baseline_c=float(body.get("baseline_c", 0.0)),
                final_c=float(body.get("final_c", 0.0)),
                raw_rise_c=float(body.get("raw_rise_c", 0.0)),
                rise_inf_c=float(body.get("rise_inf_c", 0.0)),
                step_ambient_c=float(body.get("step_ambient_c", 0.0)),
                proposed_kp=float(body.get("proposed_kp", 0.0)),
                proposed_ki=float(body.get("proposed_ki", 0.0)),
                proposed_kd=float(body.get("proposed_kd", 0.0)),
                abort_reason=body.get("abort_reason", ""),
            ))
    return rows


def parse_trace_csv(path: str) -> tuple[list[float], list[float]]:
    """Parse ``/api/autotune/trace.csv`` (``elapsed_s,measurement_c``)."""
    t: list[float] = []
    y: list[float] = []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                t.append(float(row["elapsed_s"]))
                y.append(float(row["measurement_c"]))
            except (KeyError, ValueError):
                continue
    return t, y


def parse_history_csv(path: str) -> list[dict]:
    """Parse ``/api/history.csv``.

    2026-09-01: the board's history ring buffer went from single-zone to
    per-zone (owner: "the duty cycle and all of the zones are not always
    visible on the web graph" -- see profile_executor.h's doc comment on
    profile_history_entry_t for the firmware-side reasoning), so the CSV's
    columns widened from a fixed ``elapsed_s,actual_c,desired_c,duty,guard``
    to ``elapsed_s,desired_c,z0_actual_c,z0_duty,z0_guard,z1_actual_c,...`` --
    one ``z<N>_actual_c,z<N>_duty,z<N>_guard`` triple per zone the firmware
    was built for, N not fixed at parse time. Detected from the header
    itself (``csv.DictReader``) rather than hard-coded, so this keeps working
    if the firmware's channel count ever changes, and each row's ``zones``
    dict is keyed by the zone index parsed out of the column name. Also
    still accepts the OLD single-zone header (``actual_c``/``duty`` with no
    ``z<N>_`` prefix) for any CSV captured before this change -- exposed
    there as zone 0, the same "lowest-indexed active zone" the old firmware
    scoped that column to.

    2026-09-02 (opus review of 3f9b1a9/2a8ff7e): a trailing ``zone_mask``
    column was added -- THIS ROW's own zone_mask, captured firmware-side
    when the sample was taken, not whatever run happens to be current when
    the CSV is fetched. Exposed as ``entry["zone_mask"]`` (an int, or
    ``None`` for a CSV captured before this column existed) via
    ``row.get()`` rather than ``row[...]`` so an older CSV with no such
    column is not silently dropped by the existing ``except (KeyError,
    ValueError)`` below.

    This CSV is a display-buffer export, not a durable firing record: the
    board's ring only holds the last HISTORY_MAX_SAMPLES *
    HISTORY_SAMPLE_PERIOD_S worth of samples (currently ~5h20m -- see
    profile_executor.h's own sizing comment), and a firing longer than that
    has its early history already fallen off the ring by the time this is
    fetched. Durable records live in the board's flash event log and the
    live debug-UART temperature feed, not here.
    """
    rows: list[dict] = []
    with open(path, "r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        fieldnames = reader.fieldnames or []
        zone_cols: dict[int, dict[str, str]] = {}
        for name in fieldnames:
            if name.startswith("z") and "_" in name:
                zi_str, _, suffix = name[1:].partition("_")
                if zi_str.isdigit():
                    zone_cols.setdefault(int(zi_str), {})[suffix] = name
        legacy_single_zone = not zone_cols and "actual_c" in fieldnames
        for row in reader:
            try:
                entry = {
                    "elapsed_s": float(row["elapsed_s"]),
                    "desired_c": float(row["desired_c"]),
                    "zones": {},
                }
                zone_mask_raw = row.get("zone_mask")
                entry["zone_mask"] = int(zone_mask_raw) if zone_mask_raw not in (None, "") else None
                if legacy_single_zone:
                    entry["zones"][0] = {
                        "actual_c": float(row["actual_c"]),
                        "duty": float(row["duty"]),
                        "guard": row.get("guard", ""),
                    }
                else:
                    for zi, cols in zone_cols.items():
                        entry["zones"][zi] = {
                            "actual_c": float(row[cols["actual_c"]]) if "actual_c" in cols else math.nan,
                            "duty": float(row[cols["duty"]]) if "duty" in cols else math.nan,
                            "guard": row.get(cols.get("guard", ""), ""),
                        }
                # Back-compat convenience fields, matching the old single-zone
                # shape, for any caller not yet updated for multi-zone: the
                # lowest-indexed zone with a genuine (non-NaN) reading in this
                # row, same "lowest-indexed active zone" convention the old
                # firmware used for its one recorded column.
                rep_zi = next((zi for zi in sorted(entry["zones"]) if not math.isnan(entry["zones"][zi]["actual_c"])), None)
                if rep_zi is not None:
                    entry["actual_c"] = entry["zones"][rep_zi]["actual_c"]
                    entry["duty"] = entry["zones"][rep_zi]["duty"]
                    entry["guard"] = entry["zones"][rep_zi]["guard"]
                else:
                    entry["actual_c"] = math.nan
                    entry["duty"] = math.nan
                    entry["guard"] = ""
                rows.append(entry)
            except (KeyError, ValueError):
                continue
    return rows


# ---------------------------------------------------------------------------
# Run splitting -- a poll-capture JSONL can have more than one run appended
# to it (the board was fired twice into the same log file). ``elapsed_s`` is
# relative to when THAT run started, so it drops back toward 0 at the start
# of every run after the first. Every consumer of a row sequence below
# assumes elapsed_s is monotonic non-decreasing -- callers must run
# ``split_runs`` first and pick a single run before windowing/statting.
# ---------------------------------------------------------------------------

def split_runs(rows: Sequence[PollRow]) -> list[list[PollRow]]:
    """Split ``rows`` into separate runs wherever ``elapsed_s`` decreases.

    A new run is detected the moment a row's ``elapsed_s`` is strictly less
    than the previous row's -- that is the signature of the firmware having
    restarted its poll-capture clock for a new firing appended to the same
    file. Returns a list of one or more non-empty row lists, in file order;
    a normal single-run file returns ``[rows]`` (well, an equal copy of it).
    """
    if not rows:
        return []
    runs: list[list[PollRow]] = [[rows[0]]]
    for r in rows[1:]:
        if r.elapsed_s < runs[-1][-1].elapsed_s:
            runs.append([])
        runs[-1].append(r)
    return runs


def _default_run_index(runs: Sequence[Sequence[PollRow]]) -> int:
    """Pick the run a caller should analyze when none is specified: the most
    recent run that actually carries zone data.

    A capture commonly ends with one or more trailing rows where the board
    has gone back to "idle" (segment_index/target_c/elapsed_s all reset to
    0, ``zones: []``) after a firing finishes or aborts -- that is itself a
    one-row "run" by the strict elapsed_s-decrease rule in ``split_runs``,
    but it carries no samples to analyze. Walk backward from the end and
    use the last run with at least one non-empty ``zones`` row; if every
    run is empty (degenerate input), fall back to the literal last run so
    callers still get *a* run rather than an IndexError.
    """
    for i in range(len(runs) - 1, -1, -1):
        if any(r.zones for r in runs[i]):
            return i
    return len(runs) - 1


# ---------------------------------------------------------------------------
# Windowing
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class Window:
    segment_index: int
    phase: str  # "ramp" | "dwell"
    start_idx: int  # index into the row list, inclusive
    end_idx: int  # inclusive
    start_s: float
    end_s: float

    @property
    def duration_s(self) -> float:
        return max(self.end_s - self.start_s, 0.0)


def build_windows(rows: Sequence[PollRow]) -> list[Window]:
    """Split a run into (segment_index, ramp|dwell) windows.

    A window boundary is any change in ``segment_index`` or ``dwelling``.
    This is the "windowed time" the coordinator asked for -- per-segment,
    ramp separated from dwell -- rather than one number for the whole run.

    ``rows`` must be a SINGLE run (``elapsed_s`` non-decreasing) -- a
    multi-run capture must be split with ``split_runs`` first and each run
    windowed separately, or windows would silently straddle a run boundary
    where ``elapsed_s`` resets toward 0.
    """
    if not rows:
        return []
    for i in range(1, len(rows)):
        if rows[i].elapsed_s < rows[i - 1].elapsed_s:
            raise ValueError(
                "build_windows() received rows spanning more than one run "
                "(elapsed_s decreases at index %d, %.1f -> %.1f) -- call "
                "split_runs() first and window each run separately"
                % (i, rows[i - 1].elapsed_s, rows[i].elapsed_s)
            )
    windows: list[Window] = []
    start_idx = 0
    cur_key = (rows[0].segment_index, rows[0].dwelling)
    for i in range(1, len(rows)):
        key = (rows[i].segment_index, rows[i].dwelling)
        if key != cur_key:
            windows.append(Window(
                segment_index=cur_key[0],
                phase="dwell" if cur_key[1] else "ramp",
                start_idx=start_idx, end_idx=i - 1,
                start_s=rows[start_idx].elapsed_s, end_s=rows[i - 1].elapsed_s,
            ))
            start_idx = i
            cur_key = key
    windows.append(Window(
        segment_index=cur_key[0],
        phase="dwell" if cur_key[1] else "ramp",
        start_idx=start_idx, end_idx=len(rows) - 1,
        start_s=rows[start_idx].elapsed_s, end_s=rows[len(rows) - 1].elapsed_s,
    ))
    return windows


# ---------------------------------------------------------------------------
# Per-window, per-zone statistics
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ZoneWindowStats:
    zone: int
    n_samples: int
    mean_error_c: float          # time-weighted, signed, actual - target
    rms_error_c: float           # time-weighted
    max_overshoot_c: float       # most positive error (actual above target)
    max_overshoot_at_s: float
    max_undershoot_c: float      # most negative error, reported as a positive magnitude
    max_undershoot_at_s: float
    iae_raw_c_s: float           # integral of |error| dt
    iae_normalized_c: float      # iae_raw / duration -- time-weighted MAE
    duty_min: float
    duty_max: float
    duty_mean: float


def window_zone_stats(window: Window, rows: Sequence[PollRow], zone: int) -> Optional[ZoneWindowStats]:
    idxs = range(window.start_idx, window.end_idx + 1)
    times = []
    errors = []
    duties = []
    for i in idxs:
        r = rows[i]
        z = r.zones.get(zone)
        if z is None or math.isnan(z.actual_c) or math.isnan(r.target_c):
            continue
        times.append(r.elapsed_s)
        errors.append(z.actual_c - r.target_c)
        duties.append(z.duty)
    if not times:
        return None

    # dt for row i is the gap to the NEXT row (trapezoid-ish, left-Riemann);
    # the last sample in the window gets 0 weight for the integral (no
    # "next" row to define its duration) but still counts for extrema/mean
    # of the plain (unweighted) kind if dt sums to zero (single-sample window).
    dts = [times[i + 1] - times[i] for i in range(len(times) - 1)] + [0.0]
    # Defensive: a negative gap means two runs got merged into one window
    # upstream (build_windows() should already have refused that). Clamp
    # rather than let it poison mean/rms/iae with a negative weight.
    assert all(dt >= 0 for dt in dts), (
        "negative dt in window_zone_stats -- rows from more than one run "
        "reached this window; split_runs() should have been called first"
    )
    dts = [max(dt, 0.0) for dt in dts]
    total_dt = sum(dts)

    if total_dt > 0:
        mean_err = sum(e * dt for e, dt in zip(errors, dts)) / total_dt
        rms_err = math.sqrt(sum((e * e) * dt for e, dt in zip(errors, dts)) / total_dt)
        iae_raw = sum(abs(e) * dt for e, dt in zip(errors, dts))
    else:
        mean_err = errors[0]
        rms_err = abs(errors[0])
        iae_raw = 0.0

    max_over_i = max(range(len(errors)), key=lambda i: errors[i])
    max_under_i = min(range(len(errors)), key=lambda i: errors[i])
    duration = window.duration_s
    # A zero (or negative, though Window.duration_s already clamps that)
    # duration means "no time elapsed in this window" -- dividing the raw
    # IAE (C*s) by 1.0 as a fallback used to silently relabel that raw C*s
    # figure as if it were degrees C, which is exactly how a merged-runs
    # window (start_s > end_s -> duration clamped to 0) produced a bogus
    # "iae_normalized_c" three orders of magnitude too large. NaN it instead
    # so a caller can't mistake it for a real number.
    iae_normalized = (iae_raw / duration) if duration > 0 else float("nan")

    return ZoneWindowStats(
        zone=zone,
        n_samples=len(times),
        mean_error_c=mean_err,
        rms_error_c=rms_err,
        max_overshoot_c=max(errors[max_over_i], 0.0),
        max_overshoot_at_s=times[max_over_i],
        max_undershoot_c=max(-errors[max_under_i], 0.0),
        max_undershoot_at_s=times[max_under_i],
        iae_raw_c_s=iae_raw,
        iae_normalized_c=iae_normalized,
        duty_min=min(duties),
        duty_max=max(duties),
        duty_mean=sum(duties) / len(duties),
    )


def zones_in_rows(rows: Sequence[PollRow]) -> list[int]:
    zones: set[int] = set()
    for r in rows:
        zones.update(r.zones.keys())
    return sorted(zones)


# ---------------------------------------------------------------------------
# Settle time / overshoot-lag after a ramp -> dwell transition
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class TransitionEvent:
    segment_index: int
    zone: int
    transition_at_s: float        # elapsed_s where dwelling first became true
    settle_time_s: Optional[float]  # None if it never settled within the window
    band_c: float
    peak_overshoot_c: Optional[float]
    peak_overshoot_at_s: Optional[float]
    duty_zero_at_s: Optional[float]
    overshoot_lag_s: Optional[float]  # peak time - duty_zero time


def ramp_to_dwell_transitions(
    rows: Sequence[PollRow], windows: Sequence[Window], zone: int, band_c: float = 1.0,
) -> list[TransitionEvent]:
    """For each ramp window immediately followed by a dwell window in the
    same segment, compute settle time and the duty-off -> temperature-peak
    lag. The lag is dead time plus stored thermal energy discharging after
    the heater turns off -- the signature of an over-drive defect.
    """
    events: list[TransitionEvent] = []
    for i, w in enumerate(windows):
        if w.phase != "ramp":
            continue
        if i + 1 >= len(windows):
            continue
        nxt = windows[i + 1]
        if nxt.phase != "dwell" or nxt.segment_index != w.segment_index:
            continue

        dwell_idxs = list(range(nxt.start_idx, nxt.end_idx + 1))
        dwell_target = None
        settle_time_s = None
        for j, idx in enumerate(dwell_idxs):
            r = rows[idx]
            z = r.zones.get(zone)
            if z is None:
                continue
            if dwell_target is None:
                dwell_target = r.target_c
            err = abs(z.actual_c - r.target_c)
            if err <= band_c:
                # confirm it stays inside the band for the rest of the window
                rest_ok = all(
                    rows[k].zones.get(zone) is not None
                    and abs(rows[k].zones[zone].actual_c - rows[k].target_c) <= band_c
                    for k in dwell_idxs[j:]
                )
                if rest_ok:
                    settle_time_s = r.elapsed_s - nxt.start_s
                    break

        # duty-off time: first sample in [ramp tail .. dwell] where duty
        # drops to (near) zero, searching from the end of the ramp forward.
        duty_zero_at = None
        search_idxs = list(range(w.start_idx, nxt.end_idx + 1))
        for idx in search_idxs:
            z = rows[idx].zones.get(zone)
            if z is not None and z.duty <= 0.02:
                duty_zero_at = rows[idx].elapsed_s
                break

        # peak temperature within the dwell window (the overshoot crest)
        peak_c = None
        peak_at = None
        for idx in dwell_idxs:
            z = rows[idx].zones.get(zone)
            if z is None:
                continue
            if peak_c is None or z.actual_c > peak_c:
                peak_c = z.actual_c
                peak_at = rows[idx].elapsed_s

        lag = None
        overshoot = None
        if peak_c is not None and dwell_target is not None:
            overshoot = peak_c - dwell_target
        if duty_zero_at is not None and peak_at is not None:
            lag = peak_at - duty_zero_at

        events.append(TransitionEvent(
            segment_index=w.segment_index,
            zone=zone,
            transition_at_s=nxt.start_s,
            settle_time_s=settle_time_s,
            band_c=band_c,
            peak_overshoot_c=overshoot,
            peak_overshoot_at_s=peak_at,
            duty_zero_at_s=duty_zero_at,
            overshoot_lag_s=lag,
        ))
    return events


# ---------------------------------------------------------------------------
# Saturation and ff_hold_infeasible episodes
# ---------------------------------------------------------------------------

def saturation_time_s(rows: Sequence[PollRow], zone: int, low: float = 0.01, high: float = 0.99) -> dict:
    """Total time (s) each zone spent with duty >= high or <= low.

    A zone pinned at full duty is not being controlled -- it is open-loop.
    """
    hi_s = 0.0
    lo_s = 0.0
    for i in range(len(rows) - 1):
        z = rows[i].zones.get(zone)
        if z is None:
            continue
        dt = rows[i + 1].elapsed_s - rows[i].elapsed_s
        if dt <= 0:
            continue
        if z.duty >= high:
            hi_s += dt
        elif z.duty <= low:
            lo_s += dt
    return {"at_or_above": hi_s, "at_or_below": lo_s}


def ff_hold_infeasible_episodes(rows: Sequence[PollRow], zone: int) -> list[tuple[float, float]]:
    """Contiguous [start_s, end_s] ranges where ff_hold_infeasible was set."""
    episodes: list[tuple[float, float]] = []
    open_start = None
    prev_end = None
    for r in rows:
        z = r.zones.get(zone)
        flagged = bool(z and z.ff_hold_infeasible)
        if flagged and open_start is None:
            open_start = r.elapsed_s
        if not flagged and open_start is not None:
            episodes.append((open_start, prev_end if prev_end is not None else open_start))
            open_start = None
        prev_end = r.elapsed_s
    if open_start is not None:
        episodes.append((open_start, prev_end))
    return episodes


# ---------------------------------------------------------------------------
# Sanity checks -- the kind that would have caught tonight's bugs
# ---------------------------------------------------------------------------

def sanity_check_firing(rows: Sequence[PollRow]) -> list[str]:
    """Return a list of human-readable warnings, empty if nothing looks off.

    ``rows`` must be a single run (see ``split_runs``); it is passed straight
    through to ``build_windows``, which raises if it spans more than one.

    Currently checks: every zone's overall (whole-run, time-weighted) mean
    error shares the same sign -- a systematic over- or under-drive rather
    than noise scattered around zero.
    """
    warnings: list[str] = []
    if not rows:
        return warnings
    windows = build_windows(rows)
    if not windows:
        return warnings
    whole_run = Window(
        segment_index=-1, phase="all",
        start_idx=0, end_idx=len(rows) - 1,
        start_s=rows[0].elapsed_s, end_s=rows[-1].elapsed_s,
    )
    signs = {}
    for zone in zones_in_rows(rows):
        stats = window_zone_stats(whole_run, rows, zone)
        if stats is None:
            continue
        signs[zone] = stats.mean_error_c
    nonzero = [v for v in signs.values() if abs(v) > 1e-6]
    if nonzero and all(v > 0 for v in nonzero):
        warnings.append(
            f"all zones run HOT on average (systematic over-drive): "
            + ", ".join(f"zone {z}: {e:+.2f} C" for z, e in signs.items())
        )
    elif nonzero and all(v < 0 for v in nonzero):
        warnings.append(
            f"all zones run COLD on average (systematic under-drive): "
            + ", ".join(f"zone {z}: {e:+.2f} C" for z, e in signs.items())
        )
    return warnings


def sanity_check_autotune(rows: Sequence[AutotuneRow], trace: Optional[tuple[list[float], list[float]]] = None) -> list[str]:
    """Checks against the firmware's own final autotune row (and, if a trace
    is supplied, against the raw measurements too)."""
    warnings: list[str] = []
    done = [r for r in rows if r.state == "done" and r.model_valid]
    if not done:
        return warnings
    r = done[-1]
    if r.final_c < r.baseline_c + r.rise_inf_c - 0.5 and r.rise_inf_c > 0:
        # fitted asymptote should be >= what was actually measured, modulo
        # the small extrapolation correction already applied
        pass  # final_c is itself the last measurement; nothing to flag here
    if trace is not None:
        t, y = trace
        if y:
            first_ys = y[: max(1, len(y) // 20)]
            opening_mean = sum(first_ys) / len(first_ys)
            if abs(opening_mean - r.baseline_c) > 2.0:
                warnings.append(
                    f"baseline_c ({r.baseline_c:.2f} C) disagrees with the trace's own "
                    f"opening samples (mean {opening_mean:.2f} C) by "
                    f"{abs(opening_mean - r.baseline_c):.2f} C"
                )
            asymptote = r.baseline_c + r.rise_inf_c
            max_y = max(y)
            if asymptote < max_y - 0.2:
                warnings.append(
                    f"fitted asymptote ({asymptote:.2f} C) is BELOW the last measured "
                    f"sample ({max_y:.2f} C) -- the extrapolation undershot the data"
                )
    return warnings


# ---------------------------------------------------------------------------
# Autotune summary + independent FOPDT refit
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class AutotuneSummary:
    zone: int
    method: str
    completed: bool
    abort_reason: str
    k_gain_c_per_duty: float
    tau_s: float
    dead_time_s: float
    baseline_c: float
    step_ambient_c: float
    final_c: float
    raw_rise_c: float
    rise_inf_c: float
    model_valid: bool
    model_settled: bool
    model_extrapolation_converged: bool
    model_tau_consistent: bool
    proposed_kp: float
    proposed_ki: float
    proposed_kd: float


def summarize_autotune(rows: Sequence[AutotuneRow]) -> Optional[AutotuneSummary]:
    if not rows:
        return None
    last = rows[-1]
    completed = last.state == "done" and last.model_valid
    return AutotuneSummary(
        zone=last.zone, method=last.method, completed=completed,
        abort_reason=last.abort_reason,
        k_gain_c_per_duty=last.k_gain_c_per_duty, tau_s=last.tau_s,
        dead_time_s=last.dead_time_s, baseline_c=last.baseline_c,
        step_ambient_c=last.step_ambient_c, final_c=last.final_c,
        raw_rise_c=last.raw_rise_c, rise_inf_c=last.rise_inf_c,
        model_valid=last.model_valid, model_settled=last.model_settled,
        model_extrapolation_converged=last.model_extrapolation_converged,
        model_tau_consistent=last.model_tau_consistent,
        proposed_kp=last.proposed_kp, proposed_ki=last.proposed_ki,
        proposed_kd=last.proposed_kd,
    )


@dataclasses.dataclass
class FopdtFit:
    k_gain_c_per_duty: Optional[float]
    tau_s: Optional[float]
    dead_time_s: Optional[float]
    baseline_c: Optional[float]
    sse: Optional[float]
    method: str  # "least_squares_grid" (no scipy) or "scipy_curve_fit"


def _fopdt_predict(t: Sequence[float], baseline: float, k_step: float, tau: float, dead_time: float) -> list[float]:
    out = []
    for ti in t:
        if ti <= dead_time or tau <= 0:
            out.append(baseline)
        else:
            out.append(baseline + k_step * (1.0 - math.exp(-(ti - dead_time) / tau)))
    return out


def _sse(y: Sequence[float], yhat: Sequence[float]) -> float:
    return sum((a - b) ** 2 for a, b in zip(y, yhat))


def refit_fopdt(t: Sequence[float], y: Sequence[float], duty_step: float = 1.0) -> FopdtFit:
    """Independently re-fit a first-order-plus-dead-time step response.

    scipy is not assumed to be installed (kept out deliberately -- this is a
    small tool, not worth the dependency). Instead: baseline is the mean of
    the first few samples; for a grid of candidate dead times, the step
    ``1 - exp(-(t-L)/tau)`` shape is linear in ``k_step`` once ``tau`` is
    fixed, so a coarse tau grid + closed-form least-squares K for each (tau,
    L) pair, refined by a local golden-ish narrowing pass, gets within a
    percent or two of a proper nonlinear fit -- adequate for a cross-check,
    not a control-system-grade identification tool.
    """
    if len(t) < 5:
        return FopdtFit(None, None, None, None, None, "least_squares_grid")

    try:
        import numpy as _np  # noqa: F401
        HAVE_SCIPY = False
        try:
            from scipy.optimize import curve_fit  # type: ignore
            HAVE_SCIPY = True
        except ImportError:
            HAVE_SCIPY = False
    except ImportError:
        HAVE_SCIPY = False

    n_base = max(1, len(y) // 20)
    baseline = sum(y[:n_base]) / n_base
    final_c = y[-1]
    rise_guess = max(final_c - baseline, 0.1)
    t_max = t[-1]

    if HAVE_SCIPY:
        import numpy as np
        from scipy.optimize import curve_fit

        def model(tt, k_step, tau, dead_time):
            tt = np.asarray(tt)
            out = np.where(tt <= dead_time, 0.0, k_step * (1.0 - np.exp(-(tt - dead_time) / np.maximum(tau, 1e-6))))
            return baseline + out

        try:
            popt, _ = curve_fit(
                model, t, y,
                p0=[rise_guess, max(t_max / 5.0, 1.0), max(t_max / 20.0, 1.0)],
                bounds=([0, 1e-3, 0], [rise_guess * 5 + 1, t_max * 5, t_max]),
                maxfev=5000,
            )
            k_step, tau, dead_time = popt
            yhat = _fopdt_predict(t, baseline, k_step, tau, dead_time)
            return FopdtFit(
                k_gain_c_per_duty=k_step / duty_step if duty_step else None,
                tau_s=tau, dead_time_s=dead_time, baseline_c=baseline,
                sse=_sse(y, yhat), method="scipy_curve_fit",
            )
        except Exception:
            pass  # fall through to the manual grid search

    # Manual grid search + local refinement, no scipy/numpy required.
    best = None
    tau_candidates = [t_max * f for f in (0.02, 0.05, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.7, 1.0, 1.5)]
    dt_candidates = [t_max * f for f in (0.0, 0.01, 0.02, 0.05, 0.08, 0.12, 0.16, 0.2, 0.25, 0.3)]

    def best_k_for(tau: float, dead_time: float) -> tuple[float, float]:
        num = 0.0
        den = 0.0
        for ti, yi in zip(t, y):
            if ti <= dead_time:
                continue
            basis = 1.0 - math.exp(-(ti - dead_time) / tau)
            num += basis * (yi - baseline)
            den += basis * basis
        if den <= 1e-9:
            return 0.0, math.inf
        k_step = num / den
        yhat = _fopdt_predict(t, baseline, k_step, tau, dead_time)
        return k_step, _sse(y, yhat)

    for tau in tau_candidates:
        for dead_time in dt_candidates:
            if tau <= 0:
                continue
            k_step, sse = best_k_for(tau, dead_time)
            if best is None or sse < best[3]:
                best = (k_step, tau, dead_time, sse)

    # Local refinement: narrow around the best grid point with a finer grid.
    k_step, tau, dead_time, sse = best
    for _ in range(3):
        tau_lo, tau_hi = tau * 0.5, tau * 1.5
        dt_lo, dt_hi = max(dead_time * 0.5, 0.0), dead_time * 1.5 + 1.0
        for tf in [tau_lo + (tau_hi - tau_lo) * i / 8 for i in range(9)]:
            for df in [dt_lo + (dt_hi - dt_lo) * i / 8 for i in range(9)]:
                if tf <= 0:
                    continue
                k2, sse2 = best_k_for(tf, df)
                if sse2 < sse:
                    k_step, tau, dead_time, sse = k2, tf, df, sse2

    return FopdtFit(
        k_gain_c_per_duty=k_step / duty_step if duty_step else None,
        tau_s=tau, dead_time_s=dead_time, baseline_c=baseline,
        sse=sse, method="least_squares_grid",
    )


# ---------------------------------------------------------------------------
# Report rendering
# ---------------------------------------------------------------------------

def _pct_diff(a: float, b: float) -> Optional[float]:
    if a in (0, None) or b is None:
        return None
    return (b - a) / a * 100.0


def render_firing_report(path: str, band_c: float = 1.0) -> dict:
    all_rows = parse_profile_exec_jsonl(path)
    if not all_rows:
        return {"error": f"no profile_exec rows parsed from {path}"}
    runs = split_runs(all_rows)
    used_run_index = _default_run_index(runs)
    rows = runs[used_run_index]  # most recent complete run in the file

    windows = build_windows(rows)
    zones = zones_in_rows(rows)

    win_reports = []
    for w in windows:
        zstats = {}
        for z in zones:
            s = window_zone_stats(w, rows, z)
            if s:
                zstats[z] = s
        win_reports.append((w, zstats))

    transitions = {z: ramp_to_dwell_transitions(rows, windows, z, band_c=band_c) for z in zones}
    saturation = {z: saturation_time_s(rows, z) for z in zones}
    ff_infeasible = {z: ff_hold_infeasible_episodes(rows, z) for z in zones}
    warnings = sanity_check_firing(rows)

    return {
        "path": path,
        "n_rows": len(rows),
        "n_zones": len(zones),
        "zones": zones,
        "band_c": band_c,
        "runs_in_file": len(runs),
        "used_run_index": used_run_index,
        "windows": win_reports,
        "transitions": transitions,
        "saturation": saturation,
        "ff_infeasible": ff_infeasible,
        "warnings": warnings,
    }


def format_firing_report_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [
        f"firing log: {report['path']}  ({report['n_rows']} polls, "
        f"{report['n_zones']} zones, settle band +/-{report['band_c']:.2f} C)",
    ]
    if report.get("runs_in_file", 1) > 1:
        lines.append(
            f"  ! file holds {report['runs_in_file']} runs (elapsed_s restarts partway "
            f"through) -- analyzing run #{report['used_run_index'] + 1} "
            f"(the most recent, {report['n_rows']} polls)"
        )
    if report["warnings"]:
        for w in report["warnings"]:
            lines.append(f"  ! {w}")

    for w, zstats in report["windows"]:
        lines.append(
            f"-- seg {w.segment_index} {w.phase:5s} "
            f"[{w.start_s:.0f}s..{w.end_s:.0f}s, {w.duration_s:.0f}s]"
        )
        for z in sorted(zstats):
            s = zstats[z]
            lines.append(
                f"    z{z}: mean_err={s.mean_error_c:+.2f}C rms={s.rms_error_c:.2f}C "
                f"over={s.max_overshoot_c:.2f}C@{s.max_overshoot_at_s:.0f}s "
                f"under={s.max_undershoot_c:.2f}C@{s.max_undershoot_at_s:.0f}s "
                f"iae_norm={s.iae_normalized_c:.3f}C "
                f"duty[{s.duty_min:.2f}..{s.duty_max:.2f}]avg={s.duty_mean:.2f} "
                f"n={s.n_samples}"
            )

    for z, events in report["transitions"].items():
        for e in events:
            settle = f"{e.settle_time_s:.0f}s" if e.settle_time_s is not None else "NEVER within window"
            lag = f"{e.overshoot_lag_s:.0f}s" if e.overshoot_lag_s is not None else "n/a"
            over = f"{e.peak_overshoot_c:+.2f}C" if e.peak_overshoot_c is not None else "n/a"
            lines.append(
                f"transition z{z} seg{e.segment_index}@{e.transition_at_s:.0f}s: "
                f"settle={settle} peak_overshoot={over} duty->0 to peak lag={lag}"
            )

    for z, sat in report["saturation"].items():
        if sat["at_or_above"] > 0 or sat["at_or_below"] > 0:
            lines.append(
                f"saturation z{z}: {sat['at_or_above']:.0f}s at duty>=0.99, "
                f"{sat['at_or_below']:.0f}s at duty<=0.01"
            )

    for z, episodes in report["ff_infeasible"].items():
        for (s, e) in episodes:
            lines.append(f"ff_hold_infeasible z{z}: {s:.0f}s..{e:.0f}s ({e - s:.0f}s)")

    return "\n".join(lines)


def render_autotune_report(jsonl_path: str, trace_path: Optional[str] = None) -> dict:
    rows = parse_autotune_jsonl(jsonl_path)
    if not rows:
        return {"error": f"no autotune rows parsed from {jsonl_path}"}
    summary = summarize_autotune(rows)
    trace = parse_trace_csv(trace_path) if trace_path else None
    refit = None
    if trace is not None and trace[0]:
        refit = refit_fopdt(trace[0], trace[1])
    warnings = sanity_check_autotune(rows, trace)
    return {"jsonl_path": jsonl_path, "trace_path": trace_path, "summary": summary,
            "refit": refit, "warnings": warnings}


def format_autotune_report_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    s: AutotuneSummary = report["summary"]
    lines = [
        f"autotune: {report['jsonl_path']}  zone {s.zone}  method={s.method}",
        f"  completed={s.completed}" + (f"  abort_reason={s.abort_reason!r}" if not s.completed else ""),
        f"  flags: valid={s.model_valid} settled={s.model_settled} "
        f"extrap_converged={s.model_extrapolation_converged} tau_consistent={s.model_tau_consistent}",
        f"  firmware fit: K={s.k_gain_c_per_duty:.4f} C/duty  tau={s.tau_s:.1f}s  L={s.dead_time_s:.1f}s",
        f"  baseline={s.baseline_c:.2f}C  step_ambient={s.step_ambient_c:.2f}C  "
        f"final={s.final_c:.2f}C  raw_rise={s.raw_rise_c:.2f}C  rise_inf={s.rise_inf_c:.2f}C "
        f"(extrapolation correction {s.rise_inf_c - s.raw_rise_c:+.2f}C)",
        f"  proposed PID: kp={s.proposed_kp:.5f} ki={s.proposed_ki:.5f} kd={s.proposed_kd:.5f}",
    ]
    if report["refit"] is not None:
        r: FopdtFit = report["refit"]
        if r.k_gain_c_per_duty is None:
            lines.append("  independent refit: too few trace samples to fit")
        else:
            kd = _pct_diff(s.k_gain_c_per_duty, r.k_gain_c_per_duty)
            td = _pct_diff(s.tau_s, r.tau_s)
            ld = _pct_diff(s.dead_time_s, r.dead_time_s)
            lines.append(
                f"  independent refit ({r.method}): K={r.k_gain_c_per_duty:.4f} "
                f"({kd:+.1f}% vs firmware)  tau={r.tau_s:.1f}s ({td:+.1f}%)  "
                f"L={r.dead_time_s:.1f}s ({ld:+.1f}%)  sse={r.sse:.2f}"
            )
    if report["warnings"]:
        for w in report["warnings"]:
            lines.append(f"  ! {w}")
    return "\n".join(lines)


def compare_firing_runs(path_a: str, path_b: str, band_c: float = 1.0) -> dict:
    """Whole-run, per-zone comparison of two firings of the same profile --
    the before/after check for "did tracking measurably improve".

    Either file may hold more than one run appended to it (``elapsed_s``
    restarting partway through, e.g. the board was fired twice into the same
    capture). Each file is split with ``split_runs`` and only its LAST
    (most recent, presumably-complete) run is compared -- mixing rows from
    two runs into one "whole run" window used to silently corrupt every
    stat derived from it (a clamped/negative duration in particular turned
    ``iae_normalized_c`` into the raw C*s integral mislabelled as C). The
    returned dict reports how many runs each file held and which index was
    used so a multi-run file is never compared silently.
    """
    all_rows_a = parse_profile_exec_jsonl(path_a)
    all_rows_b = parse_profile_exec_jsonl(path_b)
    if not all_rows_a or not all_rows_b:
        return {"error": "one or both runs had no parseable rows"}

    runs_a = split_runs(all_rows_a)
    runs_b = split_runs(all_rows_b)
    used_a = _default_run_index(runs_a)
    used_b = _default_run_index(runs_b)
    rows_a = runs_a[used_a]
    rows_b = runs_b[used_b]

    def whole_run_stats(rows):
        w = Window(-1, "all", 0, len(rows) - 1, rows[0].elapsed_s, rows[-1].elapsed_s)
        return {z: window_zone_stats(w, rows, z) for z in zones_in_rows(rows)}

    stats_a = whole_run_stats(rows_a)
    stats_b = whole_run_stats(rows_b)
    zones = sorted(set(stats_a) & set(stats_b))
    diffs = {}
    for z in zones:
        a, b = stats_a[z], stats_b[z]
        if a is None or b is None:
            continue
        diffs[z] = {
            "iae_normalized_a": a.iae_normalized_c, "iae_normalized_b": b.iae_normalized_c,
            "iae_normalized_improved": b.iae_normalized_c < a.iae_normalized_c,
            "rms_error_a": a.rms_error_c, "rms_error_b": b.rms_error_c,
            "mean_error_a": a.mean_error_c, "mean_error_b": b.mean_error_c,
            "max_overshoot_a": a.max_overshoot_c, "max_overshoot_b": b.max_overshoot_c,
        }
    return {
        "path_a": path_a, "path_b": path_b, "zones": zones, "diffs": diffs,
        "runs_in_a": len(runs_a), "runs_in_b": len(runs_b),
        "used_run_index_a": used_a, "used_run_index_b": used_b,
    }


def format_compare_report_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [f"compare: A={report['path_a']}  B={report['path_b']}"]
    if report.get("runs_in_a", 1) > 1:
        lines.append(
            f"  ! A holds {report['runs_in_a']} runs -- comparing run "
            f"#{report['used_run_index_a'] + 1} (the most recent)"
        )
    if report.get("runs_in_b", 1) > 1:
        lines.append(
            f"  ! B holds {report['runs_in_b']} runs -- comparing run "
            f"#{report['used_run_index_b'] + 1} (the most recent)"
        )
    for z, d in report["diffs"].items():
        arrow = "improved" if d["iae_normalized_improved"] else "WORSE"
        lines.append(
            f"  z{z}: iae_norm A={d['iae_normalized_a']:.3f}C B={d['iae_normalized_b']:.3f}C ({arrow})  "
            f"rms A={d['rms_error_a']:.2f}C B={d['rms_error_b']:.2f}C  "
            f"mean A={d['mean_error_a']:+.2f}C B={d['mean_error_b']:+.2f}C  "
            f"max_over A={d['max_overshoot_a']:.2f}C B={d['max_overshoot_b']:.2f}C"
        )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# JSON serialization (for --json / the MCP tool's json_output=True)
# ---------------------------------------------------------------------------

def _jsonable(obj):
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return {k: _jsonable(v) for k, v in dataclasses.asdict(obj).items()}
    if isinstance(obj, Window):
        return {
            "segment_index": obj.segment_index, "phase": obj.phase,
            "start_s": obj.start_s, "end_s": obj.end_s, "duration_s": obj.duration_s,
        }
    if isinstance(obj, dict):
        return {str(k): _jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_jsonable(v) for v in obj]
    if isinstance(obj, float) and math.isnan(obj):
        return None
    return obj


def firing_report_to_json(report: dict) -> str:
    if "error" in report:
        return json.dumps({"error": report["error"]})
    out = dict(report)
    out["windows"] = [
        {"window": _jsonable(w), "zones": _jsonable(zstats)} for w, zstats in report["windows"]
    ]
    out["transitions"] = _jsonable(report["transitions"])
    return json.dumps(out, indent=2)


def autotune_report_to_json(report: dict) -> str:
    return json.dumps(_jsonable(report), indent=2)


def compare_report_to_json(report: dict) -> str:
    return json.dumps(_jsonable(report), indent=2)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-log-analysis",
        description=__doc__.splitlines()[0] if __doc__ else "kilnCtl firing/tuning log analysis",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_firing = sub.add_parser("firing", help="windowed analysis of a profile_exec poll capture")
    p_firing.add_argument("jsonl_path")
    p_firing.add_argument("--band", type=float, default=1.0, help="settle band in C (default 1.0)")
    p_firing.add_argument("--json", action="store_true")

    p_auto = sub.add_parser("autotune", help="summarize + independently re-fit an autotune")
    p_auto.add_argument("jsonl_path")
    p_auto.add_argument("--trace", dest="trace_path", default=None, help="path to trace.csv for the independent refit")
    p_auto.add_argument("--json", action="store_true")

    p_cmp = sub.add_parser("compare", help="diff two firings of the same profile")
    p_cmp.add_argument("path_a")
    p_cmp.add_argument("path_b")
    p_cmp.add_argument("--band", type=float, default=1.0)
    p_cmp.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "firing":
        report = render_firing_report(args.jsonl_path, band_c=args.band)
        print(firing_report_to_json(report) if args.json else format_firing_report_text(report))
    elif args.cmd == "autotune":
        report = render_autotune_report(args.jsonl_path, args.trace_path)
        print(autotune_report_to_json(report) if args.json else format_autotune_report_text(report))
    elif args.cmd == "compare":
        report = compare_firing_runs(args.path_a, args.path_b, band_c=args.band)
        print(compare_report_to_json(report) if args.json else format_compare_report_text(report))
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
