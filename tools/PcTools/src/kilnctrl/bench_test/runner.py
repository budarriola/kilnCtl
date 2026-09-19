"""Run lifecycle: preflight -> ordered case execution -> teardown (plan §2.4).

Wave 0 only ever runs read-only cases (the `smoke` suite and whatever of
`static`/`nightly`/`full` this wave has implemented judge functions for),
so the teardown half of this lifecycle mostly has nothing to undo -- it is
still implemented in full (idle checks, board_before/after snapshots)
because later waves that DO write board state plug into this same
`BenchTestRunner` rather than a new one.
"""
from __future__ import annotations

import dataclasses
import time
import traceback
from typing import Any, Dict, List, Optional

from .registry import CaseResult, Verdict, get_case, suite_case_ids
from . import report as report_mod


def _safe_call(fn, *args, **kwargs) -> "tuple[bool, Any]":
    """True/result on success, False/str(exc) on any exception -- a
    preflight/teardown probe must never itself crash the run."""
    try:
        return True, fn(*args, **kwargs)
    except Exception as exc:  # noqa: BLE001
        return False, f"{type(exc).__name__}: {exc}"


@dataclasses.dataclass
class RunOutcome:
    run_id: str
    suite: str
    requested: List[str]
    executed: List[str]
    results: "Dict[str, CaseResult]"
    started: float
    ended: float
    preflight_ok: bool
    preflight_reason: str
    tainted: bool = False

    @property
    def exit_code(self) -> int:
        """Mirrors run_all_checks.ps1's three-way contract (bench_test.ps1
        uses this): 0 all-PASS, 3 nothing failed but something was
        SKIP/INCONCLUSIVE, 1 otherwise."""
        if not self.preflight_ok:
            return 1
        verdicts = [r.verdict for r in self.results.values()]
        if any(v == Verdict.FAIL for v in verdicts):
            return 1
        if any(v in (Verdict.SKIP, Verdict.INCONCLUSIVE, Verdict.NOT_RUN) for v in verdicts):
            return 3
        return 0


class BenchTestRunner:
    """Owns one run: mint the run directory, preflight, execute the
    requested cases in the fixed §5.2 order, teardown, and write
    summary.json/transcript.md. Board access happens only through `ctx`'s
    `srv` module and http-client helpers -- see cases_smoke.py."""

    def __init__(self, ctx: dict, logs_root: Optional[str] = None):
        self.ctx = ctx
        self.logs_root = logs_root
        self.transcript: List[str] = []

    def _log(self, line: str) -> None:
        self.transcript.append(line)

    # -- preflight (plan §2.4 step 1) --------------------------------------

    def preflight(self) -> "tuple[bool, str, dict]":
        """Best-effort, never raises: any probe that itself fails to reach
        the board is reported as a preflight failure rather than crashing
        the run. dry_run still executes preflight -- it is a read-only
        health check regardless."""
        ctx = self.ctx
        reasons: List[str] = []
        board_before: Dict[str, Any] = {}

        srv = ctx.get("srv")
        if srv is None:
            try:
                from .. import mcp_server as srv  # noqa: PLC0415
                ctx["srv"] = srv
            except Exception as exc:  # noqa: BLE001
                return False, f"could not import kilnctrl.mcp_server: {exc}", board_before

        ok, banner = _safe_call(srv._stale_banner)
        if ok and banner:
            reasons.append(f"MCP server reports stale: {banner.strip()}")

        ok, exec_status = _safe_call(srv.profiles_get_exec_status)
        board_before["profiles_get_exec_status"] = exec_status if ok else f"error: {exec_status}"
        if ok and isinstance(exec_status, str) and "RUNNING" in exec_status.upper():
            reasons.append("profiles_get_exec_status is not idle")

        ok, at_status = _safe_call(srv.autotune_get_status)
        board_before["autotune_get_status"] = at_status if ok else f"error: {at_status}"
        if ok and isinstance(at_status, str) and "running" in at_status.lower():
            reasons.append("autotune_get_status is active")

        ok, safety_status = _safe_call(srv.safety_get_status)
        board_before["safety_get_status"] = safety_status if ok else f"error: {safety_status}"
        if ok and isinstance(safety_status, str):
            if "down" in safety_status.lower():
                reasons.append("safety link is not up")
            if "trip_reason" in safety_status.lower() and "trip_reason: 0" not in safety_status.lower() and "trip_reason:0" not in safety_status.lower():
                # A latched trip stops the run for a human unless it is
                # exactly S6a mid-clear, which no wave-0 case attempts.
                if "trip_reason 0" not in safety_status.lower():
                    reasons.append("a safety trip appears to be latched; operator must clear it")

        ok, crash = _safe_call(srv.get_heap_status, ctx.get("host"))
        board_before["get_heap_status"] = crash if ok else f"error: {crash}"
        if ok and isinstance(crash, str) and "UNACKNOWLEDGED CRASH REPORT" in crash:
            reasons.append("unacknowledged crash report present")

        preflight_ok = not reasons
        return preflight_ok, "; ".join(reasons), board_before

    # -- teardown (plan §2.4 step 3) ---------------------------------------

    def teardown(self) -> dict:
        """try/finally-called by run(); read-only in wave 0 (no case here
        writes board state), so this only takes the closing snapshot -- the
        stop/restore calls are present so later waves' writing cases share
        this exact teardown rather than inventing their own."""
        ctx = self.ctx
        srv = ctx.get("srv")
        board_after: Dict[str, Any] = {}
        if srv is not None:
            ok, exec_status = _safe_call(srv.profiles_get_exec_status)
            if ok and isinstance(exec_status, str) and "RUNNING" in exec_status.upper():
                _safe_call(srv.profiles_stop)
            ok, at_status = _safe_call(srv.autotune_get_status)
            if ok and isinstance(at_status, str) and "running" in at_status.lower():
                _safe_call(srv.autotune_abort)
            ok, heap = _safe_call(srv.get_heap_status, ctx.get("host"))
            board_after["get_heap_status"] = heap if ok else f"error: {heap}"
            ok, safety_status = _safe_call(srv.safety_get_status)
            board_after["safety_get_status"] = safety_status if ok else f"error: {safety_status}"
        return board_after

    # -- run ----------------------------------------------------------------

    def run(self, suite: str, cases: Optional[List[str]] = None, dry_run: bool = False,
             allow_heat: bool = True, tag: Optional[str] = None) -> RunOutcome:
        requested = suite_case_ids(suite)
        if cases:
            requested = [c for c in requested if c in cases] or list(cases)

        run_id = report_mod.make_run_id(suite, tag)
        started = time.time()
        self._log(f"# bench_test run {run_id} (suite={suite}, dry_run={dry_run})")
        self._log(f"requested cases: {', '.join(requested)}")

        preflight_ok, preflight_reason, board_before = self.preflight()
        self._log(f"preflight: {'OK' if preflight_ok else 'FAILED - ' + preflight_reason}")

        results: Dict[str, CaseResult] = {}
        executed: List[str] = []

        if not preflight_ok:
            for cid in requested:
                results[cid] = CaseResult(Verdict.NOT_RUN, reason=f"preflight failed: {preflight_reason}")
        else:
            for cid in requested:
                spec = get_case(cid)
                if spec.heat and not allow_heat:
                    results[cid] = CaseResult(Verdict.SKIP, reason="allow_heat=False")
                    continue
                if spec.depends_on:
                    dep = results.get(spec.depends_on)
                    if dep is not None and dep.verdict != Verdict.PASS:
                        results[cid] = CaseResult(
                            Verdict.NOT_RUN,
                            reason=f"dependency {spec.depends_on} was {dep.verdict}",
                        )
                        continue
                if spec.judge is None:
                    results[cid] = CaseResult(Verdict.NOT_RUN, reason="not_implemented")
                    continue
                if dry_run:
                    results[cid] = CaseResult(Verdict.SKIP, reason="dry_run")
                    continue
                t0 = time.time()
                self._log(f"## {cid} -- {spec.description}")
                try:
                    result = spec.judge(self.ctx)
                except Exception as exc:  # noqa: BLE001 - a case must never crash the run
                    result = CaseResult(
                        Verdict.FAIL,
                        reason=f"case raised {type(exc).__name__}: {exc}",
                    )
                    self._log(traceback.format_exc())
                executed.append(cid)
                dt = time.time() - t0
                self._log(f"verdict: {result.verdict} ({dt:.2f}s) {result.reason}".rstrip())
                results[cid] = result

        board_after = self.teardown()
        ended = time.time()

        outcome = RunOutcome(
            run_id=run_id,
            suite=suite,
            requested=requested,
            executed=executed,
            results=results,
            started=started,
            ended=ended,
            preflight_ok=preflight_ok,
            preflight_reason=preflight_reason,
        )

        run_dir = report_mod.run_dir_path(self.logs_root, run_id)
        summary = report_mod.build_summary(outcome, board_before, board_after)
        report_mod.write_run(run_dir, summary, self.transcript)
        outcome_dict = dataclasses.asdict(outcome)
        outcome_dict["run_dir"] = run_dir
        outcome_dict["exit_code"] = outcome.exit_code
        outcome_dict["summary"] = summary
        self._last_run_dir = run_dir
        return outcome


def run_suite(suite: str, cases: Optional[List[str]] = None, dry_run: bool = False,
              allow_heat: bool = True, ap_password: Optional[str] = None,
              tag: Optional[str] = None, host: Optional[str] = None,
              logs_root: Optional[str] = None) -> RunOutcome:
    """Convenience entry point -- what mcp_server_bench_test.py's
    bench_test_run() calls. `ap_password` is accepted for parity with the
    plan's signature but never stored in ctx beyond this call and never
    written to summary.json/transcript.md (plan §6 rule 9); wave 0's
    read-only cases do not need it at all."""
    ctx: Dict[str, Any] = {"host": host, "tag": tag}
    runner = BenchTestRunner(ctx, logs_root=logs_root)
    return runner.run(suite=suite, cases=cases, dry_run=dry_run, allow_heat=allow_heat, tag=tag)
