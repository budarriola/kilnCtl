"""MCP surface for the READ-ONLY per-surface HTTP latency soak
(latency_soak.py / latency_soak_stats.py). GET requests only."""
from __future__ import annotations

from typing import Optional

from mcpkit import build_jobs

from . import latency_soak as _ls
from . import mcp_server_core as _core
from .latency_soak_stats import format_report

#: A synchronous call must finish under the MCP client's 300 s idle watchdog.
SYNC_MAX_S = 240.0


def _run(host: str, duration_s: float, interval_s: float, stall_ms: float,
         min_surfaces: int) -> str:
    samples = _ls.run_soak(host, duration_s, interval_s)
    return format_report(samples, stall_ms, min_surfaces,
                         f"latency soak host={host} duration_s={duration_s:g} (GET only)")


def _resolve(host: Optional[str]) -> str:
    from .mcp_server_ota import _ota_resolve_host  # local import: circular, same convention as totp_enroll_status
    return _ota_resolve_host(host)


@_core._tool()
def latency_soak(duration_s: float = 120.0, interval_s: float = 5.0, stall_ms: float = 1000.0,
                 min_surfaces: int = 3, host: Optional[str] = None) -> str:
    """READ-ONLY: poll ten GET surfaces (status, profile_exec, readiness, ota_status,
    profiles, zones, control, autotune, network/lwip, display) every `interval_s` for
    `duration_s` (max 240 here; use `latency_soak_start` for longer) and report
    per-surface p50/p95/max latency and errors, plus "clustered stall" ticks where
    >= `min_surfaces` surfaces were slower than `stall_ms` (or got no answer) in the
    same tick. Built to catch the never-reproduced 2026-08 multi-surface stall. No POSTs."""
    if not 0 < duration_s <= SYNC_MAX_S:
        return f"error: duration_s must be in (0, {SYNC_MAX_S:g}]; use latency_soak_start for longer"
    return _run(_resolve(host), duration_s, interval_s, stall_ms, min_surfaces)


@_core._tool()
def latency_soak_start(duration_s: float = 1800.0, interval_s: float = 5.0, stall_ms: float = 1000.0,
                       min_surfaces: int = 3, host: Optional[str] = None) -> str:
    """READ-ONLY background twin of `latency_soak` (default 30 min): returns a job id at
    once; poll with `bench_test_job_status(job_id, wait_s=100)` (same job registry). The
    report is the job's result. A server restart loses a run still going."""
    if duration_s <= 0:
        return "error: duration_s must be > 0"
    resolved = _resolve(host)
    job_id = build_jobs.start_job(
        "latency_soak", lambda: _run(resolved, duration_s, interval_s, stall_ms, min_surfaces),
        {"host": resolved, "duration_s": duration_s},
        classify=lambda r: "failed" if r.startswith("error") else "ok")
    return (f"soak-job {job_id}: STARTED (latency_soak, host={resolved}, {duration_s:g}s). Poll "
            f"bench_test_job_status(job_id=\"{job_id}\", wait_s=100).")
