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

import os
import re
import time
from typing import Optional

from mcpkit import build_jobs

from . import mcp_server_core as _core
from .bench_test import board_lock as bt_board_lock
from .bench_test import registry as bt_registry
from .bench_test import report as bt_report
from .bench_test.runner import RUNNER_LOG_NAME, BenchTestRunner


@_core._tool()
def bench_test_run(suite: str, cases: Optional[str] = None, dry_run: bool = False,
                    allow_heat: bool = True, lcd_stop_heat: bool = False,
                    lcd_edit_heat: bool = False, ota_allow_heat: bool = False,
                    tag: Optional[str] = None, host: Optional[str] = None,
                    attended: bool = False, allow_flash: bool = False,
                    ota_image_path: Optional[str] = None, ota_corrupt_image_path: Optional[str] = None,
                    ota_truncated_image_path: Optional[str] = None,
                    ota_wrong_build_image_path: Optional[str] = None,
                    ota_image_build: Optional[str] = None,
                    ota_pico_image_path: Optional[str] = None,
                    ota_pico_image_commit: Optional[str] = None,
                    ota_pico_corrupt_image_path: Optional[str] = None,
                    update_downgrade_repo: Optional[str] = None,
                    update_wrong_repo: Optional[str] = None) -> str:
    """Run a standardized bench-test suite against this board
    (docs/BENCH_TEST_SYSTEM_PLAN.md). `suite` is one of `smoke`, `static`,
    `flash`, `stack`, `ota`, `autotune`, `heat`, `web`, `lcd`, `safety`,
    `nightly`, or `full` -- see `bench_test_list()` for the live catalogue.
    `cases` is an optional comma-separated list of explicit case ids to
    narrow to. `dry_run=True` runs preflight and reports every requested
    case as SKIP without touching the board further. `allow_heat=False`
    SKIPs any case marked as heat-originating (wave 0 has none in `smoke`,
    but later suites do). `lcd_stop_heat=True` is a separate, default-False
    opt-in that lets LCD-19's stop_gated sub-check start a real bench firing
    to probe whether Stop is PIN-gated while heating (owner decision
    2026-09-30) -- LCD-19 is not spec.heat-marked, so `allow_heat` alone
    never gates it; both flags must be true for that firing to start.
    `lcd_edit_heat=True` is the same kind of separate, default-False
    opt-in for LCD-22/23/24 (Edit firing live edit, steppers and
    refusals, end-of-firing), each of which starts its own low-temperature
    firing. They are spec.heat-marked, so
    `allow_heat=False` skips it, but `allow_heat` defaults True, so
    without `lcd_edit_heat=True` it returns NOT_RUN naming that parameter.

    `ota_allow_heat=True` is the same kind of separate, default-False opt-in
    for suite `ota`'s OT-E07/OT-E08, which start their own firing/autotune
    to prove an OTA push is refused during one; they need it in addition to
    `allow_heat` (prefer `ota_matrix_run(allow_heat=True)`, which has a
    confirm gate and an ARMED/interlock preflight).

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

    The `ota_*` image paths / commit / build and `update_*_repo` arguments are
    the same inputs `ota_matrix_run` takes; they are put into the case context
    unchanged so suite `ota` cases that need an image (OT-E01, OT-G01..G06, ...)
    can run from here too. A case still gates itself, and still SKIPs when its
    input is absent. Nothing here relaxes a gate.

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
    for key, value in (("ota_image_path", ota_image_path),
                       ("ota_corrupt_image_path", ota_corrupt_image_path),
                       ("ota_truncated_image_path", ota_truncated_image_path),
                       ("ota_wrong_build_image_path", ota_wrong_build_image_path),
                       ("ota_image_build", ota_image_build),
                       ("ota_pico_image_path", ota_pico_image_path),
                       ("ota_pico_image_commit", ota_pico_image_commit),
                       ("ota_pico_corrupt_image_path", ota_pico_corrupt_image_path),
                       ("update_downgrade_repo", update_downgrade_repo),
                       ("update_wrong_repo", update_wrong_repo)):
        if value is not None:
            ctx[key] = value
    runner = BenchTestRunner(ctx)
    try:
        outcome = runner.run(suite=suite, cases=case_list, dry_run=dry_run,
                              allow_heat=allow_heat, lcd_stop_heat=lcd_stop_heat,
                              lcd_edit_heat=lcd_edit_heat, ota_allow_heat=ota_allow_heat, tag=tag)
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
    if outcome.tainted:
        lines.append("TAINTED: a case could not restore board state; check the board before trusting it")
    lines.append(f"run dir: {outcome.run_dir}")
    return bt_report._redact("\n".join(lines))


@_core._tool()
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


@_core._tool()
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


# --- background runs: start now, poll later --------------------------------
#
# A long suite outlasts the MCP client's 300 s idle watchdog (the run keeps
# going server-side, the caller loses the result). bench_test_start /
# bench_test_job_status mirror build_kilnfw_start / build_job_status on the
# same mcpkit.build_jobs registry. The job body calls the REAL bench_test_run /
# ota_matrix_run tool function with the caller's arguments unchanged, so the
# board lock, preflight, confirm and allow_heat gates all still run, exactly
# once, inside that call -- nothing here re-implements or skips one.

_EXIT_CODE_RE = re.compile(r"exit_code=(\d+)")
_PROGRESS_TAIL_LINES = 12


def classify_bench_report(report: str) -> str:
    """Job state from a bench_test_run/ota_matrix_run report: ``ok`` only for
    exit_code 0 (all PASS), ``incomplete`` for 3 (nothing FAILed but something
    SKIPped/INCONCLUSIVE/NOT_RUN), else ``failed`` (including every ``error:``
    refusal, which has no exit_code line)."""
    first = (report or "").splitlines()[0] if report else ""
    m = _EXIT_CODE_RE.search(first)
    if not m or first.startswith("error"):
        return "failed"
    code = int(m.group(1))
    return "ok" if code == 0 else "incomplete" if code == 3 else "failed"


def _runner_log_progress(started: float, suite: str, logs_root: Optional[str] = None) -> str:
    """Tail of runner.log of the newest run directory for ``suite`` created at
    or after ``started``. A placeholder when the runner has not made one yet
    (still in preflight/host resolution) -- never raises."""
    root = logs_root or bt_report.default_logs_root()
    safe = re.sub(r"[^A-Za-z0-9_-]", "_", suite)
    try:
        candidates = [os.path.join(root, n) for n in os.listdir(root)
                      if f"_{safe}" in n and os.path.isdir(os.path.join(root, n))]
        candidates = [c for c in candidates if os.path.getmtime(c) >= started - 2.0]
        if not candidates:
            return "(no run directory yet: runner still starting)"
        run_dir = max(candidates, key=os.path.getmtime)
        with open(os.path.join(run_dir, RUNNER_LOG_NAME), encoding="utf-8", errors="replace") as fh:
            tail = fh.read().splitlines()[-_PROGRESS_TAIL_LINES:]
    except OSError:
        return "(runner.log not readable yet)"
    return bt_report._redact(f"run dir: {run_dir}\n" + "\n".join(tail))


@_core._tool()
def bench_test_start(suite: str, cases: Optional[str] = None, dry_run: bool = False,
                      allow_heat: bool = True, lcd_stop_heat: bool = False,
                      lcd_edit_heat: bool = False, ota_allow_heat: bool = False,
                      tag: Optional[str] = None, host: Optional[str] = None,
                      attended: bool = False, allow_flash: bool = False,
                      ota_image_path: Optional[str] = None, ota_corrupt_image_path: Optional[str] = None,
                      ota_truncated_image_path: Optional[str] = None,
                      ota_wrong_build_image_path: Optional[str] = None,
                      ota_image_build: Optional[str] = None,
                      ota_pico_image_path: Optional[str] = None,
                      ota_pico_image_commit: Optional[str] = None,
                      ota_pico_corrupt_image_path: Optional[str] = None,
                      update_downgrade_repo: Optional[str] = None,
                      update_wrong_repo: Optional[str] = None) -> str:
    """Start `bench_test_run` in the background and return a job id at once.

    Same arguments, same meaning, same gating as `bench_test_run` (read its
    docstring): the job runs that very tool function, so the board lock,
    preflight and every opt-in flag (`allow_heat`, `lcd_stop_heat`,
    `lcd_edit_heat`, `ota_allow_heat`, `allow_flash`) apply unchanged, and a
    refusal (`error: refused -- ...`) comes back as the job's FAILED report.
    Use this for any suite that can outlast the MCP client's 300 s idle
    watchdog. Poll with `bench_test_job_status(job_id, wait_s=100)`; the run
    keeps going if you stop polling. A server restart loses a run that was
    still going (check logs/bench_test/<run>/runner.log)."""
    started = time.time()
    job_id = build_jobs.start_job(
        f"bench_test:{suite}",
        lambda: bench_test_run(suite=suite, cases=cases, dry_run=dry_run, allow_heat=allow_heat,
                               lcd_stop_heat=lcd_stop_heat, lcd_edit_heat=lcd_edit_heat,
                               ota_allow_heat=ota_allow_heat, tag=tag, host=host,
                               attended=attended, allow_flash=allow_flash,
                               ota_image_path=ota_image_path,
                               ota_corrupt_image_path=ota_corrupt_image_path,
                               ota_truncated_image_path=ota_truncated_image_path,
                               ota_wrong_build_image_path=ota_wrong_build_image_path,
                               ota_image_build=ota_image_build,
                               ota_pico_image_path=ota_pico_image_path,
                               ota_pico_image_commit=ota_pico_image_commit,
                               ota_pico_corrupt_image_path=ota_pico_corrupt_image_path,
                               update_downgrade_repo=update_downgrade_repo,
                               update_wrong_repo=update_wrong_repo),
        {"suite": suite, "cases": cases, "dry_run": dry_run, "allow_heat": allow_heat,
         "tag": tag},
        classify=classify_bench_report,
        progress=lambda: _runner_log_progress(started, suite))
    return (f"bench-job {job_id}: STARTED (bench_test:{suite}). Poll "
            f"bench_test_job_status(job_id=\"{job_id}\", wait_s=100); the run keeps "
            f"going if you stop polling.")


@_core._tool()
def bench_test_job_status(job_id: str, wait_s: float = 0.0) -> str:
    """Report a background run started by `bench_test_start` or
    `ota_matrix_start`: RUNNING (with the tail of the run's runner.log), then
    OK (exit_code 0) / INCOMPLETE (exit_code 3: skips/inconclusive) / FAILED,
    plus the same report the synchronous tool prints. `wait_s` blocks up to
    that many seconds (capped at 120, under the client idle watchdog) for
    completion. Results survive a server restart in a file under the temp
    `kilnctl-builds` directory; a run still going at restart reports unknown."""
    return build_jobs.job_status(job_id, wait_s, noun="bench-job", poll_tool="bench_test_job_status")

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
