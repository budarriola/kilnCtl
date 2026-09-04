"""Parser for the HTTP-capture poll format used by the fuzzy-layer A/B runs
(``logs/coupling/p7_fuzzy0_http.jsonl`` / ``p7_fuzzy50_http.jsonl``).

FILE SHAPE. One JSON object per line, no wall-clock text prefix (unlike
``log_analysis.parse_profile_exec_jsonl``'s ``HH:MM:SS {...}`` captures):

    {"t": <unix float>,
     "exec": {<verbatim GET /api/profile_exec response body>},
     "status": {<verbatim GET /api/status response body>}}

``exec`` is the same shape ``log_analysis.parse_profile_exec_jsonl`` already
parses out of a poll-capture line -- it is handed straight to
``log_analysis.poll_row_from_exec_body`` so this module owns exactly one
thing (locating that body and a wall-clock label inside the new envelope)
and every downstream tool (windowing, IAE, transitions, comparisons)
consumes it unmodified. ``status`` is parsed and kept on ``HttpPollRow`` for
callers that want it (e.g. a starting-ambient / heap / link-health cross
check) but is not used by anything in this module -- the PID A/B comparison
only needs the ``exec`` half.

ELAPSED_S. Unlike ``coupling_pair_log.py``'s exec-status *text* capture,
whose ``elapsed=Ns`` field resets to 0 at every ramp/dwell transition, this
HTTP capture's ``exec.elapsed_s`` is the run-total figure (confirmed against
the real ``p7_fuzzy0_http.jsonl``: it climbs monotonically straight through
the ramp -> dwell boundary at row 19, ``627 -> 642 -> ...``, while the
sibling field ``segment_elapsed_s`` is the one that resets). So
``elapsed_s`` is taken directly from the body via
``poll_row_from_exec_body`` -- no wall-clock-relative reconstruction needed
here, unlike the pair-log parser.

Lines that are not this shape (a blank line, a malformed line, or a
``{"t","status"}``-only line with no ``exec`` -- neither should appear in a
real capture but a flaky link is not to be ruled out) are skipped, matching
``parse_profile_exec_jsonl``'s own tolerance for a truncated poll capture.
"""
from __future__ import annotations

import dataclasses
import datetime
import json
import os
from typing import Optional, Sequence

from kilnctrl import log_analysis as la
from kilnctrl.jsonl_util import iter_jsonl


@dataclasses.dataclass
class HttpPollRow:
    """One line of an HTTP-capture poll: the parsed ``PollRow`` plus the raw
    ``status`` body it was captured alongside, and the original unix
    timestamp (``PollRow.wall_time`` is a rendered string, not sortable
    back to a float without reparsing)."""
    t: float
    poll: la.PollRow
    status: Optional[dict]


def _wall_time(t: float) -> str:
    return datetime.datetime.fromtimestamp(t, tz=datetime.timezone.utc).strftime("%H:%M:%S")


def parse_http_capture_jsonl(path: str) -> list[HttpPollRow]:
    """Parse an HTTP-capture ``{"t","exec","status"}`` JSONL file into
    ``HttpPollRow`` records (one per line with a valid ``exec`` body)."""
    rows: list[HttpPollRow] = []
    for obj in iter_jsonl(path, on_error="skip"):
        if not isinstance(obj, dict):
            continue
        body = obj.get("exec")
        if not isinstance(body, dict) or "zones" not in body or "dwelling" not in body:
            continue
        t = obj.get("t")
        try:
            t = float(t)
        except (TypeError, ValueError):
            continue
        poll = la.poll_row_from_exec_body(_wall_time(t), body)
        rows.append(HttpPollRow(t=t, poll=poll, status=obj.get("status")))
    return rows


def poll_rows(path: str) -> list[la.PollRow]:
    """Convenience: just the ``PollRow`` sequence, the shape every other
    analysis function in ``log_analysis.py`` (and ``pid_ab_compare.py``)
    consumes."""
    return [r.poll for r in parse_http_capture_jsonl(path)]


def split_http_capture_lines(path: str) -> list[list[str]]:
    """Split a raw HTTP-capture JSONL file into one list of raw lines per
    run, so a multi-run capture (e.g. a poller left running across a kiln
    cooldown into the next firing -- the actual near-miss this function was
    written for) can be turned into one clean single-run file per firing
    without hand-editing.

    Boundary detection mirrors ``log_analysis.split_runs``: a new run starts
    wherever a line's ``exec.elapsed_s`` is less than the previous exec
    line's. This works at the RAW LINE level (not the parsed ``PollRow``
    level) so the split files are byte-identical excerpts of the original --
    no reserialization, no risk of dropping a field nothing here happens to
    read (e.g. the ``status`` half of the envelope). A line with no
    parseable ``exec.elapsed_s`` (blank, malformed, or a bare
    ``{"t","status"}`` line) never starts a new run on its own; it rides
    along with whichever run is currently open.
    """
    runs: list[list[str]] = []
    last_elapsed: Optional[float] = None
    with open(path, "r", encoding="utf-8") as fh:
        for raw_line in fh:
            line = raw_line.rstrip("\n")
            stripped = line.strip()
            elapsed: Optional[float] = None
            if stripped.startswith("{"):
                try:
                    obj = json.loads(stripped)
                except json.JSONDecodeError:
                    obj = None
                if isinstance(obj, dict):
                    body = obj.get("exec")
                    if isinstance(body, dict):
                        try:
                            elapsed = float(body.get("elapsed_s"))
                        except (TypeError, ValueError):
                            elapsed = None
            if elapsed is not None and (last_elapsed is None or elapsed < last_elapsed):
                runs.append([])
            if elapsed is not None:
                last_elapsed = elapsed
            if not runs:
                runs.append([])
            runs[-1].append(line)
    return [r for r in runs if r]


def write_split_runs(path: str, outdir: str, prefix: Optional[str] = None) -> list[str]:
    """Write each run in ``path`` to its own file under ``outdir``, named
    ``<prefix>_run<N>.jsonl`` (N starting at 1, file order). Returns the list
    of paths written, in run order. This is the first-class version of the
    ad-hoc split every prior multi-run capture has needed by hand -- see
    ``pid_ab_compare split`` for the CLI entry point.

    Raises the same ``FileNotFoundError``/``OSError`` a caller would get
    from opening ``path`` directly; returns ``[]`` (writes nothing) if the
    file has no lines to split.
    """
    lines_per_run = split_http_capture_lines(path)
    if not lines_per_run:
        return []
    if prefix is None:
        prefix = os.path.splitext(os.path.basename(path))[0]
    os.makedirs(outdir, exist_ok=True)
    out_paths = []
    for i, lines in enumerate(lines_per_run, start=1):
        out_path = os.path.join(outdir, f"{prefix}_run{i}.jsonl")
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        out_paths.append(out_path)
    return out_paths


def starting_temps_c(rows: Sequence[la.PollRow]) -> dict:
    """First-sample ``actual_c`` per zone -- the run's starting temperature,
    used to size the residual-heat confounder in ``pid_ab_compare.py``.
    Returns ``{}`` if ``rows`` is empty."""
    if not rows:
        return {}
    first = rows[0]
    return {z: s.actual_c for z, s in first.zones.items()}
