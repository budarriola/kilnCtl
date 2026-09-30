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

from typing import Optional

from . import mcp_server as _srv
from .bench_test import board_lock as bt_board_lock
from .bench_test import registry as bt_registry
from .bench_test import report as bt_report
from .bench_test.runner import BenchTestRunner


@_srv._tool()
def bench_test_run(suite: str, cases: Optional[str] = None, dry_run: bool = False,
                    allow_heat: bool = True,
                    tag: Optional[str] = None, host: Optional[str] = None,
                    attended: bool = False, allow_flash: bool = False) -> str:
    """Run a standardized bench-test suite against this board
    (docs/BENCH_TEST_SYSTEM_PLAN.md). `suite` is one of `smoke`, `static`,
    `flash`, `stack`, `ota`, `autotune`, `heat`, `web`, `lcd`, `safety`,
    `nightly`, or `full` -- see `bench_test_list()` for the live catalogue.
    `cases` is an optional comma-separated list of explicit case ids to
    narrow to. `dry_run=True` runs preflight and reports every requested
    case as SKIP without touching the board further. `allow_heat=False`
    SKIPs any case marked as heat-originating (wave 0 has none in `smoke`,
    but later suites do).

    Every OTA/factory-reset/sw-reset route this can drive is ROUTE_TIER_ADMIN
    only, on or off, since the AP-password HMAC challenge/response scheme
    they used to also require was retired 2026-09-29 (WEB_AUTH_PLAN.md item
    2b) -- no separate credential parameter is accepted here any more.

    Writes one run directory under `logs/bench_test/<UTC timestamp>_
    <suite>[_<tag>]/` (summary.json, transcript.md, board_before/after.json)
    and returns a short text summary plus that run's exit-code contract
    (0 all-PASS; 1 a case FAILed; 2 preflight refused the run outright;
    3 nothing FAILed but something was SKIP/INCONCLUSIVE/NOT_RUN -- see
    `RunOutcome.exit_code`'s docstring and `tools/bench_test.ps1`'s header
    for the full rationale). Also appends one line to
    `docs/BENCH_TEST_LOG.md` per run (never a credential) via
    `report.append_log_line()`.

    `attended=True` (plan §6, Wave 3b) says a human is physically present at
    the bench, so operator-only cases (SP-08, SP-09, WEB-WIFI-06) ask a
    yes/no question instead of SKIPping with reason "requires --attended" --
    see bench_test/operator.py. `allow_flash=True` opts into FL-10/FL-11
    (an ESP/Pico JTAG flash round trip); both stay SKIP without it,
    independent of `attended`, since a flash needs no operator present.

    No case in this wave heats, flashes, writes config, or touches Wi-Fi,
    unless it was explicitly opted into as above.

    `host=None` (the default) is resolved the same way `get_heap_status()`
    resolves it -- explicit host wins (not applicable here), else the
    board's current STA IP, else the fallback-AP address
    (`mcp_server_ota._ota_resolve_host_with_source`) -- before it is ever put
    into `ctx`. Without this, every HTTP-using case reads `ctx["host"]` as
    the literal `None` and fails with a DNS/getaddrinfo error rather than
    falling back to the board's address. Resolving with no explicit `host`
    makes one read-only UART `wifi.get_status()` call (to check for a
    connected STA IP) even under `dry_run=True` -- the only board contact
    a dry run makes. The report always names the resolved host and how it
    was resolved (`explicit`/`STA IP`/`default`), since the `default`
    fallback is a possibly-stale cached address, not necessarily the board
    actually on the bench right now."""
    from .mcp_server_ota import _ota_resolve_host_with_source  # local import: avoids a circular import with mcp_server_ota.py

    resolved_host, host_source = _ota_resolve_host_with_source(host)
    if not resolved_host:
        return "error: could not resolve a board host (no explicit host, no STA IP, no AP default)"
    case_list = [c.strip() for c in cases.split(",") if c.strip()] if cases else None
    ctx = {"host": resolved_host, "tag": tag,
           "attended": attended, "allow_flash": allow_flash}
    runner = BenchTestRunner(ctx)
    try:
        outcome = runner.run(suite=suite, cases=case_list, dry_run=dry_run,
                              allow_heat=allow_heat, tag=tag)
    except (KeyError, ValueError) as exc:
        return f"error: {exc}"
    except bt_board_lock.BoardLockHeld as exc:
        # Fail closed, before preflight ever ran (docs/audits/
        # profile_executor_panic_2026-09-24.md HP-02/HP-05): another
        # mutating run already owns the board.
        return f"error: refused -- {exc}"

    lines = [f"bench_test_run: suite={suite} run_id={outcome.run_id} exit_code={outcome.exit_code}",
             f"host: {resolved_host} ({host_source})"]
    if not outcome.preflight_ok:
        lines.append(f"PREFLIGHT FAILED: {outcome.preflight_reason}")
    for cid in outcome.requested:
        result = outcome.results.get(cid)
        if result is None:
            continue
        reason = f" -- {result.reason}" if result.reason else ""
        lines.append(f"  {cid}: {result.verdict}{reason}")
    lines.append(f"run dir: {outcome.run_dir}")
    return bt_report._redact("\n".join(lines))


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
