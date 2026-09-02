"""Parser for the single-zone coupling-excitation capture format -- two
parallel 20 s pollers writing independent JSON-lines files, joined here into
the same ``log_analysis.PollRow`` shape every other module in this package
already consumes (see ``log_analysis.py``'s own "SOURCE KINDS" note: a new
telemetry source only has to produce ``PollRow``/``ZoneSample``, nothing
downstream has to change).

FILE PAIR. For a run named e.g. ``cpl_z0``:

  * ``<name>_mcp.jsonl`` -- one JSON line per poll, ``{"t": <unix float>,
    "s": <text>}`` where ``s`` is the verbatim text of a
    ``profiles_get_exec_status`` MCP tool reply:

        state=1 profile=#4 'cpl_z0' segment=0/1 dwelling=True target=55.0C
        elapsed=600s dwell_remaining=1500s ramp_lock=False fault_guard=0
          zone 0: mode=2 actual=56.1C (valid) duty=0.63 relay=on faulted=False

    followed by one indented "zone N: ..." line per zone the profile's
    ``zone_mask`` is actually driving -- a single-zone excitation profile
    (as these are) has exactly one such line, the ACTIVE zone. Peer
    (passive) zones are simply absent from this file; they are recovered
    from the thermo log below.

  * ``<name>_thermo.jsonl`` -- one JSON line per poll, same ``{"t", "s"}``
    shape, where ``s`` is the verbatim text of a ``thermo_read`` reply:

        CH0: 56.88 C (CJ 26.31 C)
        CH1: 33.81 C (CJ 26.47 C)
        CH2: 29.61 C (CJ 26.58 C)

    ``CHn`` is zone ``n``'s thermocouple channel; the CJ (cold-junction)
    reading is parsed but not currently used (see ``coupled_ident.py``'s
    "AMBIENT REFERENCE" note -- this module deliberately does not
    substitute CJ for the run's-own-first-sample ambient convention
    everything else uses, so behaviour stays uniform across sources).

JOIN. The two files are independent pollers and are NOT sample-aligned --
each exec-status row is paired with the thermo row whose timestamp is
CLOSEST to it, subject to ``max_skew_s`` (default 15 s, comfortably under
half the 20 s poll period so a row is never paired across a full sample
gap). An exec-status row with no thermo row inside that window is dropped
(not emitted with missing peer data) -- see module docstring's "PollRow
zones" below for why a wholesale drop, rather than a partial row, is the
safe choice here.

POLLROW ASSEMBLY. Each joined pair becomes one ``PollRow`` whose ``zones``
dict holds:

  * the ACTIVE zone(s) named in the exec-status line, with their real
    ``duty`` (and ``actual_c`` taken from the exec line itself, not the
    thermo line -- the two sensors can disagree by the quantization visible
    in the fixtures, and the exec line's ``actual_c`` is what the firmware's
    own settle/duty logic actually acted on);
  * every OTHER zone (0/1/2 minus the active set), with ``actual_c`` from
    the matched thermo row's ``CHn`` and ``duty=0.0`` -- these are genuinely
    passive in a single-zone excitation firing (that is the whole point of
    the experiment), and ``duty=0.0`` is exactly what
    ``single_zone_column_observations``'s ``require_min_duty=False`` path
    for passive zones expects, never mistaken for a real driven-zone
    reading because that path never checks a passive zone's duty at all.

An ``actual=nanC (invalid)`` exec-status reading (thermocouple fault) is
parsed as ``float('nan')``, matching ``log_analysis.py``'s own convention,
so every downstream NaN-skip already in ``coupled_ident.py`` (e.g.
``_zone_settle_row``'s ``math.isnan`` guard) applies unchanged.

ELAPSED_S IS NOT THE TEXT'S ``elapsed=Ns`` FIELD. That field resets to 0 at
every phase change (ramp -> dwell), confirmed against the real cpl_z0
capture: elapsed climbs ...,795s,815s,835s while ``dwelling=False``, then
the very next line reads ``dwelling=True ... elapsed=0s``. Both
``log_analysis.build_windows`` (raises if ``elapsed_s`` ever decreases
within a run) and ``split_runs`` (treats a decrease as a NEW RUN) assume
``elapsed_s`` is monotonic non-decreasing for the whole run, so feeding the
text field straight into ``PollRow.elapsed_s`` would silently fragment
every phase boundary into a fake new "run". Instead, ``PollRow.elapsed_s``
here is derived from the poller's own wall-clock ``t``, seconds since the
run's first exec-status sample -- monotonic by construction. The parsed
``elapsed=Ns`` text field is kept on ``ExecStatusSample`` for reference but
never propagated into a ``PollRow``.
"""
from __future__ import annotations

import bisect
import dataclasses
import json
import math
import re
from typing import Optional, Sequence

from . import log_analysis as la

ZONES = (0, 1, 2)

_HEADER_RE = re.compile(
    r"state=(?P<state>\S+)\s+profile=#(?P<profile>\d+)\s+'(?P<name>[^']*)'\s+"
    r"segment=(?P<seg>\d+)/(?P<segcount>\d+)\s+dwelling=(?P<dwelling>True|False)\s+"
    r"target=(?P<target>[-\d.]+)C\s+elapsed=(?P<elapsed>\d+)s\s+"
    r"dwell_remaining=(?P<remain>\d+)s\s+ramp_lock=(?P<ramplock>\S+)\s+"
    r"fault_guard=(?P<fault>\d+)"
)
_ZONE_RE = re.compile(
    r"zone\s+(?P<zone>\d+):\s+mode=(?P<mode>\d+)\s+"
    r"actual=(?P<actual>nan|[-\d.]+)C\s+\((?P<valid>valid|invalid)\)\s+"
    r"duty=(?P<duty>[-\d.]+)\s+relay=(?P<relay>\S+)\s+faulted=(?P<faulted>\S+)"
)
_THERMO_RE = re.compile(
    r"CH(?P<ch>\d+):\s+(?P<temp>nan|[-\d.]+)\s+C\s+\(CJ\s+(?P<cj>nan|[-\d.]+)\s+C\)"
)

DEFAULT_MAX_SKEW_S = 15.0


@dataclasses.dataclass
class ExecStatusSample:
    t: float
    state: str
    profile: int
    name: str
    segment_index: int
    segment_count: int
    dwelling: bool
    target_c: float
    text_elapsed_s: float  # the "elapsed=Ns" field verbatim -- resets per phase, see module docstring
    dwell_remaining_s: float
    zones: dict  # zone -> ZoneSample (active zones only, straight off the exec line)


@dataclasses.dataclass
class ThermoSample:
    t: float
    channels: dict  # zone -> actual_c


def _parse_float(text: str) -> float:
    return float("nan") if text == "nan" else float(text)


def parse_exec_status_text(text: str) -> Optional[dict]:
    """Parse one ``profiles_get_exec_status`` reply body into its fields,
    or ``None`` if the header line doesn't match (e.g. a truncated/garbled
    capture line -- callers skip these rather than raising, matching
    ``log_analysis.parse_profile_exec_jsonl``'s own tolerance for a flaky
    capture link).
    """
    m = _HEADER_RE.search(text)
    if m is None:
        return None
    zones = {}
    for zm in _ZONE_RE.finditer(text):
        z = int(zm.group("zone"))
        actual = _parse_float(zm.group("actual"))
        if zm.group("valid") == "invalid":
            actual = float("nan")
        zones[z] = la.ZoneSample(zone=z, actual_c=actual, duty=float(zm.group("duty")))
    return dict(
        state=m.group("state"),
        profile=int(m.group("profile")),
        name=m.group("name"),
        segment_index=int(m.group("seg")),
        segment_count=int(m.group("segcount")),
        dwelling=(m.group("dwelling") == "True"),
        target_c=float(m.group("target")),
        text_elapsed_s=float(m.group("elapsed")),
        dwell_remaining_s=float(m.group("remain")),
        zones=zones,
    )


def parse_thermo_text(text: str) -> Optional[dict]:
    """Parse one ``thermo_read`` reply body into ``{zone: actual_c}``, or
    ``None`` if no ``CHn`` lines matched at all.
    """
    channels = {}
    for m in _THERMO_RE.finditer(text):
        channels[int(m.group("ch"))] = _parse_float(m.group("temp"))
    if not channels:
        return None
    return channels


def _load_jsonl(path: str) -> list:
    """Read ``{"t": ..., "s": ...}`` lines, skipping any that fail to
    parse (blank/truncated trailing line from a still-being-written
    capture -- same tolerance ``log_analysis`` documents elsewhere)."""
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "t" not in obj or "s" not in obj:
                continue
            out.append((float(obj["t"]), obj["s"]))
    return out


def load_exec_status_log(path: str) -> list[ExecStatusSample]:
    out = []
    for t, text in _load_jsonl(path):
        parsed = parse_exec_status_text(text)
        if parsed is None:
            continue
        out.append(ExecStatusSample(t=t, **parsed))
    out.sort(key=lambda s: s.t)
    return out


def load_thermo_log(path: str) -> list[ThermoSample]:
    out = []
    for t, text in _load_jsonl(path):
        channels = parse_thermo_text(text)
        if channels is None:
            continue
        out.append(ThermoSample(t=t, channels=channels))
    out.sort(key=lambda s: s.t)
    return out


def load_thermo_samples_any_format(path: str) -> list[ThermoSample]:
    """Same result as ``load_thermo_log`` (per-zone actual_c timeseries),
    but also accepts the OTHER raw temperature-capture format seen in
    ``logs/coupling/`` cooldown captures: one ``kiln_io_get_status()``-
    style status JSON object per line, prefixed with a plain ``HH:MM:SS``
    wall-clock stamp (``"23:32:34 {...}"``), timestamped by its own
    ``time_now_epoch`` field rather than an outer ``{"t": ...}`` envelope,
    and carrying temperatures under ``channels: [{"channel": n, "temp_c":
    x, "valid": bool}, ...]`` instead of a ``thermo_read`` reply string.
    Tried first as the ``{"t", "s"}`` pair-log format; a line that fails
    that (no ``{`` at the start, or missing ``t``/``s`` keys) falls back
    to the status-JSON format. A line matching neither is skipped, same
    tolerance as ``_load_jsonl``.
    """
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            sample = None
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                obj = None
            if obj is not None and "t" in obj and "s" in obj:
                channels = parse_thermo_text(obj["s"])
                if channels is not None:
                    sample = ThermoSample(t=float(obj["t"]), channels=channels)
            if sample is None:
                brace = line.find("{")
                if brace > 0:
                    try:
                        status = json.loads(line[brace:])
                    except json.JSONDecodeError:
                        status = None
                    if status is not None and "channels" in status and "time_now_epoch" in status:
                        channels = {c["channel"]: c["temp_c"] for c in status["channels"] if c.get("valid")}
                        if channels:
                            sample = ThermoSample(t=float(status["time_now_epoch"]), channels=channels)
            if sample is not None:
                out.append(sample)
    out.sort(key=lambda s: s.t)
    return out


def _nearest_thermo(thermo: Sequence[ThermoSample], times: Sequence[float], t: float,
                     max_skew_s: float) -> Optional[ThermoSample]:
    """Nearest-by-timestamp match, ``None`` if the closest candidate is
    farther than ``max_skew_s`` away. ``times`` is ``[s.t for s in thermo]``,
    pre-sorted, passed in so a caller joining many rows doesn't re-derive it
    every call."""
    if not thermo:
        return None
    i = bisect.bisect_left(times, t)
    candidates = []
    if i < len(thermo):
        candidates.append(thermo[i])
    if i > 0:
        candidates.append(thermo[i - 1])
    if not candidates:
        return None
    best = min(candidates, key=lambda s: abs(s.t - t))
    if abs(best.t - t) > max_skew_s:
        return None
    return best


def load_pair_run(mcp_path: str, thermo_path: str,
                   max_skew_s: float = DEFAULT_MAX_SKEW_S) -> list[la.PollRow]:
    """Join a single run's exec-status and thermo logs into ``PollRow``s --
    see module docstring for the assembly rule. Rows whose exec-status
    sample has no thermo match within ``max_skew_s`` are dropped (a
    ``PollRow`` with silently-missing peer zones would masquerade as a
    complete reading to every downstream consumer, which never checks for
    that).
    """
    execs = load_exec_status_log(mcp_path)
    thermo = load_thermo_log(thermo_path)
    thermo_times = [s.t for s in thermo]
    if not execs:
        return []
    t0 = execs[0].t  # see module docstring's "ELAPSED_S IS NOT ..." note

    rows: list[la.PollRow] = []
    for e in execs:
        th = _nearest_thermo(thermo, thermo_times, e.t, max_skew_s)
        if th is None:
            continue
        zones = dict(e.zones)  # active zone(s), verbatim off the exec line
        for z in ZONES:
            if z in zones:
                continue
            actual = th.channels.get(z)
            if actual is None:
                continue
            zones[z] = la.ZoneSample(zone=z, actual_c=actual, duty=0.0)
        if any(z not in zones for z in ZONES):
            continue  # incomplete peer coverage for this row -- drop it whole
        rows.append(la.PollRow(
            wall_time="", elapsed_s=e.t - t0, segment_index=e.segment_index,
            segment_count=e.segment_count, dwelling=e.dwelling, target_c=e.target_c,
            state=e.state, zones=zones,
        ))
    rows.sort(key=lambda r: r.elapsed_s)
    return rows
