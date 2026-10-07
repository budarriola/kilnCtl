#!/usr/bin/env python3
"""latency_soak_stats.py -- pure statistics and "clustered stall" detection for
the per-surface HTTP latency soak (latency_soak.py). No sockets, no clock, no
board: everything here takes already-collected :class:`Sample` records, so it is
unit-tested in tools/PcTools/tests/test_latency_soak_stats.py.

Motivation: a one-off 2026-08 stall hit several UI/HTTP surfaces at once and was
never reproduced (KilnFW TODO). A single slow surface is a slow handler; several
surfaces slow in the SAME polling window points at something shared (httpd
worker pool, a lock, the heap, the network stack). The soak polls every surface
once per tick, so a tick is the window, and a "clustered stall" tick is one in
which at least ``min_surfaces`` surfaces were stalled.

A sample is *stalled* when its latency exceeds the threshold, or when it never
got an HTTP answer at all (timeout / unreachable). An HTTP error answer that came
back fast (401, 404, 500) is an error but not a stall.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional


@dataclass(frozen=True)
class Sample:
    tick: int
    surface: str
    latency_ms: float
    ok: bool                      # got a 2xx answer
    status: Optional[int] = None  # HTTP status; None = no answer (timeout/unreachable)
    error: str = ""


@dataclass(frozen=True)
class SurfaceStats:
    surface: str
    n: int
    errors: int
    p50_ms: Optional[float]
    p95_ms: Optional[float]
    max_ms: Optional[float]


@dataclass(frozen=True)
class ClusterTick:
    tick: int
    stalled: "tuple[str, ...]"
    worst_ms: float


@dataclass(frozen=True)
class Episode:
    """Consecutive clustered ticks merged into one event."""
    first_tick: int
    last_tick: int
    surfaces: "tuple[str, ...]"
    worst_ms: float

    @property
    def ticks(self) -> int:
        return self.last_tick - self.first_tick + 1


def percentile(values: Iterable[float], pct: float) -> Optional[float]:
    """Linear-interpolated percentile (numpy 'linear' method). None if empty."""
    xs = sorted(values)
    if not xs:
        return None
    if not 0.0 <= pct <= 100.0:
        raise ValueError("pct must be within [0, 100]")
    if len(xs) == 1:
        return xs[0]
    rank = (len(xs) - 1) * pct / 100.0
    lo = math.floor(rank)
    hi = math.ceil(rank)
    return xs[lo] + (xs[hi] - xs[lo]) * (rank - lo)


def summarize(samples: Iterable[Sample]) -> Dict[str, SurfaceStats]:
    """Per-surface count, error count and p50/p95/max latency. A timeout's
    latency is the timeout, not a handler measurement, but it is included so a
    stall is never hidden from max."""
    by: Dict[str, List[Sample]] = {}
    for s in samples:
        by.setdefault(s.surface, []).append(s)
    out: Dict[str, SurfaceStats] = {}
    for name, group in by.items():
        lat = [s.latency_ms for s in group]
        out[name] = SurfaceStats(
            surface=name, n=len(group), errors=sum(1 for s in group if not s.ok),
            p50_ms=percentile(lat, 50), p95_ms=percentile(lat, 95),
            max_ms=max(lat) if lat else None)
    return out


def is_stalled(s: Sample, threshold_ms: float) -> bool:
    return s.latency_ms > threshold_ms or s.status is None


def find_clustered_ticks(samples: Iterable[Sample], threshold_ms: float,
                         min_surfaces: int) -> List[ClusterTick]:
    """Ticks in which >= ``min_surfaces`` DISTINCT surfaces were stalled."""
    if min_surfaces < 1:
        raise ValueError("min_surfaces must be >= 1")
    per_tick: Dict[int, Dict[str, float]] = {}
    for s in samples:
        if is_stalled(s, threshold_ms):
            cur = per_tick.setdefault(s.tick, {})
            cur[s.surface] = max(cur.get(s.surface, 0.0), s.latency_ms)
    return [ClusterTick(tick=t, stalled=tuple(sorted(d)), worst_ms=max(d.values()))
            for t, d in sorted(per_tick.items()) if len(d) >= min_surfaces]


def merge_episodes(clusters: Iterable[ClusterTick]) -> List[Episode]:
    """Merge clustered ticks with consecutive tick numbers into episodes."""
    out: List[Episode] = []
    cur: Optional[dict] = None
    for c in sorted(clusters, key=lambda c: c.tick):
        if cur is not None and c.tick == cur["last"] + 1:
            cur["last"] = c.tick
            cur["surfaces"] |= set(c.stalled)
            cur["worst"] = max(cur["worst"], c.worst_ms)
        else:
            if cur is not None:
                out.append(_episode(cur))
            cur = {"first": c.tick, "last": c.tick, "surfaces": set(c.stalled),
                   "worst": c.worst_ms}
    if cur is not None:
        out.append(_episode(cur))
    return out


def _episode(cur: dict) -> Episode:
    return Episode(cur["first"], cur["last"], tuple(sorted(cur["surfaces"])), cur["worst"])


def format_report(samples: "List[Sample]", threshold_ms: float, min_surfaces: int,
                  header: str = "") -> str:
    stats = summarize(samples)
    ticks = len({s.tick for s in samples})
    lines = [header] if header else []
    lines.append(f"ticks={ticks} samples={len(samples)} stall_threshold_ms={threshold_ms:g} "
                 f"cluster_min_surfaces={min_surfaces}")
    lines.append(f"{'surface':<14}{'n':>6}{'err':>5}{'p50_ms':>9}{'p95_ms':>9}{'max_ms':>9}")
    for name in sorted(stats):
        st = stats[name]
        lines.append(f"{name:<14}{st.n:>6}{st.errors:>5}{_f(st.p50_ms):>9}{_f(st.p95_ms):>9}{_f(st.max_ms):>9}")
    errs: Dict[str, str] = {}
    for s in samples:
        if not s.ok and s.surface not in errs:
            errs[s.surface] = f"HTTP {s.status}" if s.status else (s.error or "no answer")
    for name in sorted(errs):
        lines.append(f"first error {name}: {errs[name]}")
    clusters = find_clustered_ticks(samples, threshold_ms, min_surfaces)
    eps = merge_episodes(clusters)
    lines.append(f"clustered stall ticks: {len(clusters)} in {len(eps)} episode(s)")
    for e in eps:
        lines.append(f"  ticks {e.first_tick}-{e.last_tick} ({e.ticks}) worst={e.worst_ms:.0f}ms "
                     f"surfaces={','.join(e.surfaces)}")
    return "\n".join(lines)


def _f(v: Optional[float]) -> str:
    return "-" if v is None else f"{v:.0f}"
