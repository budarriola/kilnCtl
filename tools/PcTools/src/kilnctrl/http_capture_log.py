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
from typing import Optional, Sequence

from kilnctrl import log_analysis as la


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
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
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


def starting_temps_c(rows: Sequence[la.PollRow]) -> dict:
    """First-sample ``actual_c`` per zone -- the run's starting temperature,
    used to size the residual-heat confounder in ``pid_ab_compare.py``.
    Returns ``{}`` if ``rows`` is empty."""
    if not rows:
        return {}
    first = rows[0]
    return {z: s.actual_c for z, s in first.zones.items()}
