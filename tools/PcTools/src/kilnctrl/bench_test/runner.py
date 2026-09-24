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

from .registry import REGISTRY, CaseResult, Verdict, get_case, suite_case_ids
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
    run_dir: str = ""

    @property
    def exit_code(self) -> int:
        """Wave 2's four-way contract (`tools/bench_test.ps1`'s header has
        the full rationale). The plan text pins only one point directly --
        §6 rule 11: a SKIP/INCONCLUSIVE case must exit distinctly from a
        FAIL, and `bench_test.ps1` "exits 3 for either". Everything else
        here (0/1/2, and NOT_RUN sharing SKIP/INCONCLUSIVE's bucket) is this
        wave's own fill-in for what §2.4/§8 leave unstated, chosen to mirror
        `run_all_checks.ps1`'s own three-way convention plus one more code
        so a refused run (never even attempted a case) is distinguishable
        from a run that attempted cases and something inside it failed:

          0 -- preflight passed and every executed case PASSed
          1 -- preflight passed but at least one case FAILed
          2 -- preflight itself refused the run (no case was attempted)
          3 -- preflight passed, nothing FAILed, but something was
               SKIP/INCONCLUSIVE/NOT_RUN (plan §6 rule 11's bucket --
               NOT_RUN added here since an unimplemented/dependency-skipped
               case is the same "incomplete, not broken" shape)
        """
        if not self.preflight_ok:
            return 2
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

        # Structured fields, not substring parsing of the display string --
        # "RUNNING" also matches inside "NOT RUNNING", and "trip_reason 0"
        # matched inside "trip_reason 10". `_running()`/`_active()` below
        # compare the actual state code the client decoded off the wire.
        ok, exec_status = _safe_call(srv._profiles.get_exec_status)
        board_before["profiles_get_exec_status"] = (
            f"state={exec_status.state_name}" if ok else f"error: {exec_status}"
        )
        if ok and exec_status.state_name == "running":
            reasons.append("profiles_get_exec_status is not idle")

        ok, at_status = _safe_call(srv._autotune.get_status)
        board_before["autotune_get_status"] = (
            f"state={at_status.state_name}" if ok else f"error: {at_status}"
        )
        if ok and at_status.state_name in ("settling", "stepping", "relay_approach", "relay_cycling"):
            reasons.append("autotune_get_status is active")

        ok, safety_status = _safe_call(srv._safety.get_status)
        board_before["safety_get_status"] = (
            f"link_up={safety_status.link_up}" if ok else f"error: {safety_status}"
        )
        if ok and not safety_status.link_up:
            reasons.append("safety link is not up")

        ok, safety_diag = _safe_call(srv._safety.get_diag)
        board_before["safety_get_diag"] = (
            f"trip_reason={safety_diag.trip_reason}" if ok else f"error: {safety_diag}"
        )
        if ok and safety_diag.ever_received and safety_diag.trip_reason != 0:
            # A latched trip stops the run for a human unless it is
            # exactly S6a mid-clear, which no wave-0 case attempts.
            reasons.append(f"a safety trip is latched (trip_reason={safety_diag.trip_reason}); operator must clear it")

        ok, crash = _safe_call(srv.get_heap_status, ctx.get("host"))
        board_before["get_heap_status"] = crash if ok else f"error: {crash}"

        # Real capability_preflight (capability_preflight.py), not a grep of
        # get_heap_status's display text for "UNACKNOWLEDGED CRASH REPORT":
        # this is the same check load-bearing for run_queue.py, so it also
        # covers readiness-gate blockers and board-unreachable, not just the
        # crash banner. An empty preset means "no HTTP-gated capability is
        # required" -- the crash/readiness checks run regardless of preset.
        # `ctx["capability_preflight_run"]` lets tests inject a fake
        # report producer instead of hitting a real board.
        from .. import capability_preflight  # noqa: PLC0415

        cp_run = ctx.get("capability_preflight_run", capability_preflight.run_preflight)
        ok, cp_report = _safe_call(
            cp_run, {}, ctx.get("host") or capability_preflight.PREFLIGHT_AP_DEFAULT_HOST,
        )
        board_before["capability_preflight"] = cp_report.describe() if ok else f"error: {cp_report}"
        # Fed to docs/BENCH_TEST_LOG.md's one-line-per-run entry
        # (report.append_log_line) -- best-effort, never a second board
        # round trip: cp_report already carries the ESP's fw_build.
        board_before["esp_fw_build"] = getattr(cp_report.board, "fw_build", None) if ok else None
        ok_pico, pico_fw = _safe_call(getattr(srv._safety, "get_fw_version", lambda: None))
        board_before["safety_get_fw_version"] = (
            pico_fw.describe() if ok_pico and hasattr(pico_fw, "describe") else
            (str(pico_fw) if ok_pico else f"error: {pico_fw}")
        )
        if ok and not cp_report.ok:
            if cp_report.board.crash_unacknowledged:
                reasons.append(f"unacknowledged crash report present: {cp_report.board.crash_summary}")
            elif cp_report.board.readiness_blocked:
                names = ", ".join(k for k, _l, _d in cp_report.board.readiness_blocked)
                reasons.append(f"readiness gate blocks: {names}")
            elif not cp_report.board.reachable:
                reasons.append(f"capability_preflight: board unreachable: {cp_report.board.error}")
            else:
                reasons.append("capability_preflight reports not ok")

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
            ok, exec_status = _safe_call(srv._profiles.get_exec_status)
            if ok and exec_status.state_name == "running":
                _safe_call(srv.profiles_stop)
            ok, at_status = _safe_call(srv._autotune.get_status)
            if ok and at_status.state_name in ("settling", "stepping", "relay_approach", "relay_cycling"):
                _safe_call(srv.autotune_abort)
            ok, heap = _safe_call(srv.get_heap_status, ctx.get("host"))
            board_after["get_heap_status"] = heap if ok else f"error: {heap}"
            ok, safety_status = _safe_call(srv._safety.get_status)
            board_after["safety_get_status"] = (
                f"link_up={safety_status.link_up}" if ok else f"error: {safety_status}"
            )
        return board_after

    # -- run ----------------------------------------------------------------

    def run(self, suite: str, cases: Optional[List[str]] = None, dry_run: bool = False,
             allow_heat: bool = True, tag: Optional[str] = None) -> RunOutcome:
        requested = suite_case_ids(suite)
        if cases:
            unknown = [c for c in cases if c not in REGISTRY]
            if unknown:
                raise ValueError(f"unknown bench-test case id(s): {', '.join(unknown)}")
            out_of_suite = [c for c in cases if c not in requested]
            if out_of_suite:
                raise ValueError(
                    f"case id(s) not in suite {suite!r}: {', '.join(out_of_suite)} "
                    f"(suite {suite!r} has: {', '.join(requested)})"
                )
            requested = [c for c in requested if c in cases]

        run_id = report_mod.make_run_id(suite, tag)
        started = time.time()
        # Made available to case judge functions via ctx["run_dir"] *before*
        # any case runs (wave 1d, SK-01/02: a case that writes a fresh
        # stack-margin baseline record needs a stable directory to write it
        # into -- the run's own directory -- rather than inventing a scratch
        # location. Deterministic from run_id/logs_root, so computing it here
        # and again below in report_mod.run_dir_path() for `outcome.run_dir`
        # always agrees.
        self.ctx["run_dir"] = report_mod.run_dir_path(self.logs_root, run_id)
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
                    if dep is None and spec.depends_on in requested:
                        # The dependency IS in this run but has not executed
                        # yet -- i.e. the suite order puts it AFTER us. The
                        # old `dep is not None` gate fell through silently
                        # here and ran the observer against a board whose
                        # dependency never happened, which reads as a real
                        # verdict but is measuring nothing. Report the
                        # ordering fault instead of fabricating a result.
                        results[cid] = CaseResult(
                            Verdict.NOT_RUN,
                            reason=(
                                f"suite order fault: dependency {spec.depends_on} is in this "
                                f"run but is scheduled after {cid}"
                            ),
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
        outcome.run_dir = run_dir
        summary = report_mod.build_summary(outcome, board_before, board_after)
        # ctx["bench_test_log_doc_path"] lets tests (and, in principle, an
        # alternate deployment) redirect docs/BENCH_TEST_LOG.md's append --
        # production code never sets it, so a real run always appends to
        # the real doc at its default path (report.default_log_doc_path()).
        report_mod.write_run(run_dir, summary, self.transcript,
                              log_doc_path=self.ctx.get("bench_test_log_doc_path"))
        self._last_run_dir = run_dir
        return outcome


def run_suite(suite: str, cases: Optional[List[str]] = None, dry_run: bool = False,
              allow_heat: bool = True, ap_password: Optional[str] = None,
              tag: Optional[str] = None, host: Optional[str] = None,
              logs_root: Optional[str] = None) -> RunOutcome:
    """Convenience entry point -- NOT what mcp_server_bench_test.py's
    bench_test_run() calls; that tool builds its own ctx directly (resolving
    `host` via `mcp_server_ota._ota_resolve_host_with_source` before it ever
    reaches `ctx`) and constructs `BenchTestRunner` itself. This function has
    no caller in this tree today; it passes `host` straight through
    UNRESOLVED (a `None` here reaches `ctx["host"]` literally, same bug
    `bench_test_run`/`ota_matrix_run` had before it was fixed) -- resolve it
    the same way before using this as a real entry point. `ap_password` is
    accepted for parity with the plan's signature but never stored in ctx
    beyond this call and never written to summary.json/transcript.md (plan
    §6 rule 9); wave 0's read-only cases do not need it at all."""
    ctx: Dict[str, Any] = {"host": host, "tag": tag}
    runner = BenchTestRunner(ctx, logs_root=logs_root)
    return runner.run(suite=suite, cases=cases, dry_run=dry_run, allow_heat=allow_heat, tag=tag)
