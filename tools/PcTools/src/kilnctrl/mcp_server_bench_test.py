"""bench_test MCP tools -- exposes tools/PcTools/src/kilnctrl/bench_test/
through the sanctioned facade, same "own module, plumbed through
mcp_server.py's star-import list" pattern mcp_server_capability_preflight.py
and mcp_server_ramp_assist.py use (see that module's docstring).

docs/BENCH_TEST_SYSTEM_PLAN.md is the design document this implements;
wave 0 (this file) only ships `smoke`'s read-only cases -- see
bench_test/registry.py's module docstring for what is and is not
implemented yet.
"""
from __future__ import annotations

import json
from typing import Optional

from . import mcp_server as _srv
from .bench_test import registry as bt_registry
from .bench_test import report as bt_report
from .bench_test.runner import BenchTestRunner


@_srv._tool()
def bench_test_run(suite: str, cases: Optional[str] = None, dry_run: bool = False,
                    allow_heat: bool = True, ap_password: Optional[str] = None,
                    tag: Optional[str] = None, host: Optional[str] = None) -> str:
    """Run a standardized bench-test suite against this board
    (docs/BENCH_TEST_SYSTEM_PLAN.md). `suite` is one of `smoke`, `static`,
    `flash`, `stack`, `ota`, `autotune`, `heat`, `web`, `lcd`, `safety`,
    `nightly`, or `full` -- see `bench_test_list()` for the live catalogue.
    `cases` is an optional comma-separated list of explicit case ids to
    narrow to. `dry_run=True` runs preflight and reports every requested
    case as SKIP without touching the board further. `allow_heat=False`
    SKIPs any case marked as heat-originating (wave 0 has none in `smoke`,
    but later suites do).

    `ap_password` is accepted for signature parity with the plan and is
    never persisted to summary.json/transcript.md (redacted before write)
    -- wave 0's read-only cases do not use it.

    Writes one run directory under `logs/bench_test/<UTC timestamp>_
    <suite>[_<tag>]/` (summary.json, transcript.md, board_before/after.json)
    and returns a short text summary plus that run's exit-code contract
    (0 all-PASS, 3 nothing failed but something SKIP/INCONCLUSIVE/NOT_RUN,
    1 otherwise -- the same three-way contract run_all_checks.ps1 uses).

    No case in this wave heats, flashes, writes config, or touches Wi-Fi."""
    case_list = [c.strip() for c in cases.split(",") if c.strip()] if cases else None
    ctx = {"host": host, "ap_password": ap_password, "tag": tag}
    runner = BenchTestRunner(ctx)
    try:
        outcome = runner.run(suite=suite, cases=case_list, dry_run=dry_run,
                              allow_heat=allow_heat, tag=tag)
    except KeyError as exc:
        return f"error: {exc}"

    lines = [f"bench_test_run: suite={suite} run_id={outcome.run_id} exit_code={outcome.exit_code}"]
    if not outcome.preflight_ok:
        lines.append(f"PREFLIGHT FAILED: {outcome.preflight_reason}")
    for cid in outcome.requested:
        result = outcome.results.get(cid)
        if result is None:
            continue
        reason = f" -- {result.reason}" if result.reason else ""
        lines.append(f"  {cid}: {result.verdict}{reason}")
    lines.append(f"run dir: {bt_report.run_dir_path(None, outcome.run_id)}")
    return "\n".join(lines)


@_srv._tool()
def bench_test_list(suite: Optional[str] = None) -> str:
    """Catalogue of bench-test cases from the case registry
    (docs/BENCH_TEST_SYSTEM_PLAN.md §3). With `suite`, lists only that
    suite's case ids in their fixed run order; with none, lists every
    known suite name and its case count. A case whose judge function is
    not implemented yet (a later wave) is marked `[not implemented]`."""
    if suite is None:
        lines = ["known suites:"]
        for name, ids in sorted(bt_registry.SUITES.items()):
            lines.append(f"  {name}: {len(ids)} cases")
        return "\n".join(lines)
    try:
        ids = bt_registry.suite_case_ids(suite)
    except KeyError as exc:
        return f"error: {exc}"
    lines = [f"suite {suite}: {len(ids)} cases"]
    for cid in ids:
        spec = bt_registry.get_case(cid)
        impl = "" if spec.judge is not None else " [not implemented]"
        heat = " [heat]" if spec.heat else ""
        lines.append(f"  {cid}: {spec.description}{heat}{impl}")
    return "\n".join(lines)


@_srv._tool()
def bench_test_last(n: int = 1) -> str:
    """Summaries of the `n` most recent bench-test runs (most recent
    first), read back from `logs/bench_test/<run>/summary.json`."""
    runs = bt_report.list_recent_runs(n=n)
    if not runs:
        return "no bench-test runs found under logs/bench_test/"
    lines = []
    for summary in runs:
        lines.append(
            f"{summary['run_id']} suite={summary['suite']} exit_code={summary.get('exit_code')} "
            f"preflight_ok={summary.get('preflight_ok')}"
        )
        for cid, case in summary.get("cases", {}).items():
            reason = f" -- {case['reason']}" if case.get("reason") else ""
            lines.append(f"  {cid}: {case['verdict']}{reason}")
    return "\n".join(lines)
