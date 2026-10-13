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
import os
import re
import threading
import time
import traceback
from typing import Any, Dict, List, Optional

from .registry import REGISTRY, CaseResult, Verdict, get_case, suite_case_ids
from . import board_lock
from . import windows
from . import report as report_mod


def _safe_call(fn, *args, **kwargs) -> "tuple[bool, Any]":
    """True/result on success, False/str(exc) on any exception -- a
    preflight/teardown probe must never itself crash the run."""
    try:
        return True, fn(*args, **kwargs)
    except Exception as exc:  # noqa: BLE001
        return False, f"{type(exc).__name__}: {exc}"


#: Wall-clock bound, in seconds, on everything that runs between taking the
#: board lock and the first case (preflight) and on teardown. Each individual
#: probe has its own serial/HTTP timeout, but the sum of ~10 probes against a
#: wedged link (or a probe that blocks on a lock, a held serial hub, or a
#: socket opened with no timeout) is unbounded -- ROADMAP B3 (2026-10-02): a
#: heat run took `.board_lock` and then sat forever with no run directory.
#: Overridable per run via ctx["preflight_timeout_s"] / ctx["teardown_timeout_s"].
DEFAULT_PREFLIGHT_TIMEOUT_S = 120.0
DEFAULT_TEARDOWN_TIMEOUT_S = 60.0

#: Name of the always-written, timestamped, flushed-per-line progress log in
#: the run directory (created right after the board lock is taken).
RUNNER_LOG_NAME = "runner.log"


class _StepTimeout(Exception):
    """A bounded step did not finish inside its deadline."""


class _Abandoned(Exception):
    """Raised inside an abandoned worker thread at its next checkpoint."""


#: Worker threads whose step timed out and were abandoned (they cannot be
#: killed). A later preflight() in this process refuses while any is still
#: alive: an unwedged worker could otherwise issue board calls into a LATER
#: run, defeating board-lock serialization.
_ABANDONED_THREADS: List[threading.Thread] = []


def _live_abandoned() -> List[threading.Thread]:
    _ABANDONED_THREADS[:] = [t for t in _ABANDONED_THREADS if t.is_alive()]
    return list(_ABANDONED_THREADS)


def _run_bounded(fn, timeout_s: float, label: str, stalled: Optional[threading.Event] = None):
    """Run `fn()` on a daemon thread and wait at most `timeout_s`. Returns
    its result, re-raises its exception, or raises `_StepTimeout`. The
    abandoned thread cannot be killed; it is a daemon so it never keeps the
    process alive, and the caller must treat the board state as unknown."""
    box: Dict[str, Any] = {}

    def _target() -> None:
        try:
            box["value"] = fn()
        except BaseException as exc:  # noqa: BLE001 - re-raised in the caller
            box["exc"] = exc

    t = threading.Thread(target=_target, name=f"bench-{label}", daemon=True)
    t.start()
    t.join(timeout_s)
    if t.is_alive():
        if stalled is not None:
            stalled.set()
        _ABANDONED_THREADS.append(t)
        raise _StepTimeout(f"{label} did not finish within {timeout_s:g} s")
    if "exc" in box:
        raise box["exc"]
    return box.get("value")


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
          1 -- preflight passed but at least one case FAILed, or the run
               is tainted (a case could not restore board state)
          2 -- preflight itself refused the run (no case was attempted)
          3 -- preflight passed, nothing FAILed, but something was
               SKIP/INCONCLUSIVE/NOT_RUN (plan §6 rule 11's bucket --
               NOT_RUN added here since an unimplemented/dependency-skipped
               case is the same "incomplete, not broken" shape)
        """
        if not self.preflight_ok:
            return 2
        verdicts = [r.verdict for r in self.results.values()]
        if any(v == Verdict.FAIL for v in verdicts) or self.tainted:
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
        self._runner_log_path: Optional[str] = None
        self._step = "not started"
        self._pf_board_before: Dict[str, Any] = {}
        #: Set once a bounded step is declared stalled; the abandoned worker
        #: checks it (via `_mark`, and before each mutating call) and stops.
        self._stalled = threading.Event()

    def _log(self, line: str) -> None:
        self.transcript.append(line)
        self._runner_log(line)

    def _runner_log(self, line: str) -> None:
        """Append one timestamped line to runner.log, flushed immediately so
        a stall leaves the last completed step on disk. Best-effort: a log
        write failure never takes down the run."""
        path = self._runner_log_path
        if path is None:
            return
        line = report_mod._redact(line)
        now = time.time()
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(now)) + f".{int(now * 1000) % 1000:03d}Z"
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(f"{stamp} {line}\n")
                f.flush()
        except OSError:
            pass

    def _mark(self, step: str) -> None:
        """Record the step about to start (named in a timeout message)."""
        if self._stalled.is_set():
            raise _Abandoned(step)
        self._step = step
        self._runner_log(f"step: {step}")

    # -- preflight (plan §2.4 step 1) --------------------------------------

    def preflight(self) -> "tuple[bool, str, dict]":
        """Bounded wrapper over `_preflight_unbounded`: a stall anywhere in
        the probes becomes a preflight FAILURE naming the step it stalled
        in, never an indefinite hang while the board lock is held."""
        timeout_s = float(self.ctx.get("preflight_timeout_s", DEFAULT_PREFLIGHT_TIMEOUT_S))
        self._pf_board_before = {}
        live = _live_abandoned()
        if live:
            reason = (
                "preflight refused: abandoned worker thread(s) from an earlier stalled "
                f"step are still alive ({', '.join(t.name for t in live)}); the board "
                "link is presumed wedged"
            )
            self._runner_log(reason)
            return False, reason, {"preflight_stall": reason}
        self._stalled = threading.Event()
        try:
            return _run_bounded(self._preflight_unbounded, timeout_s, "preflight", self._stalled)
        except _StepTimeout as exc:
            reason = f"preflight stalled: {exc} (last step: {self._step})"
            self._runner_log(reason)
            board_before = dict(self._pf_board_before)
            board_before["preflight_stall"] = reason
            return False, reason, board_before

    def _preflight_unbounded(self) -> "tuple[bool, str, dict]":
        """Best-effort, never raises: any probe that itself fails to reach
        the board is reported as a preflight failure rather than crashing
        the run. dry_run still executes preflight -- it is a read-only
        health check regardless."""
        ctx = self.ctx
        reasons: List[str] = []
        board_before: Dict[str, Any] = {}
        self._pf_board_before = board_before

        self._mark("preflight: resolve server module")
        srv = ctx.get("srv")
        if srv is None:
            try:
                from .. import mcp_server as srv  # noqa: PLC0415
                if self._stalled.is_set():
                    raise _Abandoned("srv import")
                ctx["srv"] = srv
            except Exception as exc:  # noqa: BLE001
                return False, f"could not import kilnctrl.mcp_server: {exc}", board_before

        # Firmware-version confirmation (2026-10-04, HP-07): the server refuses
        # every non-INFO send -- safety_clear_trip() included -- until
        # get_fw_version has succeeded once in this server session (it sets
        # `_srv._info.compatible`). Call the SAME MCP tool function, exactly
        # once, before any case so a case that clears a trip works on a fresh
        # server; the gate itself is untouched. The reported commit is
        # recorded for provenance. A srv without the tool (unit-test fakes)
        # is skipped, not failed.
        self._mark("preflight: get_fw_version")
        fw_tool = getattr(srv, "get_fw_version", None)
        if fw_tool is None:
            board_before["fw_version"] = "skipped: srv has no get_fw_version"
            board_before["fw_commit"] = None
        else:
            ok, fw_text = _safe_call(fw_tool)
            fw_text = str(fw_text)
            m = re.search(r"^commit:\s*(\S+)", fw_text, re.MULTILINE)
            board_before["fw_commit"] = m.group(1) if m else None
            board_before["fw_version"] = fw_text if ok else f"error: {fw_text}"
            if not ok or fw_text.lstrip().startswith("error:"):
                reasons.append(f"get_fw_version failed (device commands would be refused): {fw_text.strip()[:200]}")
            elif re.search(r"^compatible:\s*NO", fw_text, re.MULTILINE):
                reasons.append("firmware UART protocol version does not match pc_tools (device commands would be refused)")

        self._mark("preflight: stale banner")
        ok, banner = _safe_call(srv._stale_banner)
        if ok and banner:
            reasons.append(f"MCP server reports stale: {banner.strip()}")

        # Structured fields, not substring parsing of the display string --
        # "RUNNING" also matches inside "NOT RUNNING", and "trip_reason 0"
        # matched inside "trip_reason 10". `_running()`/`_active()` below
        # compare the actual state code the client decoded off the wire.
        self._mark("preflight: profiles_get_exec_status")
        ok, exec_status = _safe_call(srv._profiles.get_exec_status)
        board_before["profiles_get_exec_status"] = (
            f"state={exec_status.state_name}" if ok else f"error: {exec_status}"
        )
        if ok and exec_status.state_name == "running":
            reasons.append("profiles_get_exec_status is not idle")

        self._mark("preflight: autotune_get_status")
        ok, at_status = _safe_call(srv._autotune.get_status)
        board_before["autotune_get_status"] = (
            f"state={at_status.state_name}" if ok else f"error: {at_status}"
        )
        if ok and at_status.state_name in ("settling", "stepping", "relay_approach", "relay_cycling"):
            reasons.append("autotune_get_status is active")

        self._mark("preflight: safety_get_status")
        ok, safety_status = _safe_call(srv._safety.get_status)
        board_before["safety_get_status"] = (
            f"link_up={safety_status.link_up}" if ok else f"error: {safety_status}"
        )
        if ok and not safety_status.link_up:
            reasons.append("safety link is not up")

        self._mark("preflight: safety_get_diag")
        ok, safety_diag = _safe_call(srv._safety.get_diag)
        board_before["safety_get_diag"] = (
            f"trip_reason={safety_diag.trip_reason}" if ok else f"error: {safety_diag}"
        )
        if ok and safety_diag.ever_received and safety_diag.trip_reason != 0:
            # A latched trip stops the run for a human unless it is
            # exactly S6a mid-clear, which no wave-0 case attempts.
            reasons.append(f"a safety trip is latched (trip_reason={safety_diag.trip_reason}); operator must clear it")

        self._mark("preflight: get_heap_status (HTTP)")
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
        self._mark("preflight: capability_preflight (HTTP)")
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
        self._mark("preflight: safety_get_fw_version")
        ok_pico, pico_fw = _safe_call(getattr(srv._safety, "get_fw_version", lambda: None))
        board_before["safety_get_fw_version"] = (
            pico_fw.describe() if ok_pico and hasattr(pico_fw, "describe") else
            (str(pico_fw) if ok_pico else f"error: {pico_fw}")
        )
        # E-stop unverified blocks HEAT cases only (capability_preflight.
        # HEAT_ONLY_BLOCKING_KEYS); fail-closed: if the report could not be
        # read at all, heat is treated as blocked too.
        self.ctx["estop_unverified"] = bool(
            (not ok) or getattr(cp_report.board, "heat_blocked", ()))
        if self.ctx["estop_unverified"]:
            from ..capability_preflight import ESTOP_UNVERIFIED_LINE  # noqa: PLC0415
            board_before["estop_unverified"] = ESTOP_UNVERIFIED_LINE
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
        """Bounded wrapper over `_teardown_unbounded` (same stall rationale
        as `preflight`); a stall is reported in board_after, not raised."""
        timeout_s = float(self.ctx.get("teardown_timeout_s", DEFAULT_TEARDOWN_TIMEOUT_S))
        self._mark("teardown")
        self._stalled = threading.Event()
        try:
            return _run_bounded(self._teardown_unbounded, timeout_s, "teardown", self._stalled)
        except _StepTimeout as exc:
            msg = f"teardown stalled: {exc}"
            self._runner_log(msg)
            return {"teardown_stall": msg}

    def _teardown_unbounded(self) -> dict:
        """try/finally-called by run(); read-only in wave 0 (no case here
        writes board state), so this only takes the closing snapshot -- the
        stop/restore calls are present so later waves' writing cases share
        this exact teardown rather than inventing their own."""
        ctx = self.ctx
        srv = ctx.get("srv")
        stalled = self._stalled
        board_after: Dict[str, Any] = {}
        if srv is not None:
            ok, exec_status = _safe_call(srv._profiles.get_exec_status)
            # An abandoned worker must never issue a mutating call: check the
            # stall flag immediately before each one.
            if ok and exec_status.state_name in ("running", "paused") and not stalled.is_set():
                _safe_call(srv.profiles_stop)
                # Poll (bounded) until the executor actually reads idle before restore hooks run.
                for _ in range(int(ctx.get("teardown_idle_polls", 10))):
                    ok2, st2 = _safe_call(srv._profiles.get_exec_status)
                    if ok2 and st2.state_name == "idle":
                        break
                    (ctx.get("sleep_fn") or time.sleep)(float(ctx.get("teardown_idle_poll_s", 0.5)))
                else:
                    board_after["teardown_executor"] = "executor not confirmed idle after stop"
                    ctx["_tainted"] = True
            # Suite-registered restore hooks (e.g. AX relay-4 aux entry) run even
            # when the run aborted between the mutating case and its restore case.
            skip_hooks = "teardown_executor" in board_after
            if skip_hooks and ctx.get("teardown_hooks"):
                board_after["teardown_hooks_skipped"] = "executor not confirmed idle; restore hooks not run"
                self._runner_log("teardown: executor not confirmed idle, skipping restore hooks")
            for hook in ([] if skip_hooks else list(ctx.get("teardown_hooks") or [])):
                if not stalled.is_set():
                    hok, herr = _safe_call(hook, ctx)
                    if not hok:
                        board_after.setdefault("teardown_hook_errors", []).append(str(herr))
                        ctx["_tainted"] = True
            ok, at_status = _safe_call(srv._autotune.get_status)
            if ok and at_status.state_name in ("settling", "stepping", "relay_approach", "relay_cycling")                     and not stalled.is_set():
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
             allow_heat: bool = True, lcd_stop_heat: bool = False,
             lcd_edit_heat: bool = False, ota_allow_heat: bool = False,
             tag: Optional[str] = None) -> RunOutcome:
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

        # A reused ctx must never inherit a previous run's taint.
        self.ctx.pop("_tainted", None)
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
        self.ctx["suite"] = suite
        # Made available to case functions via ctx["allow_heat"] --
        # `spec.heat` gates a whole case (skipped outright above when
        # False), but LCD-19 is not `heat`-flagged (its other sub-checks --
        # keypad raise, wrong/right PIN -- never touch the board's heaters)
        # and must stay runnable without `allow_heat`. `allow_heat` defaults
        # True (an ordinary run may exercise other, spec.heat-marked cases),
        # so it is NOT by itself a safe gate for LCD-19's stop_gated
        # sub-check, which starts an unsolicited firing of its own. That
        # sub-check instead requires a second, independently-defaulted-False
        # opt-in, ctx["lcd19_allow_heat"] (`lcd_stop_heat` here) -- the case
        # body only starts its bench firing when BOTH flags are true, so
        # heat stays opt-in for LCD-19 even though allow_heat=True is the
        # default for everything else.
        self.ctx["allow_heat"] = allow_heat
        self.ctx["suite"] = suite
        self.ctx["lcd19_allow_heat"] = lcd_stop_heat
        # LCD-22/23/24 (Edit firing live edit) ARE spec.heat-marked, so a False
        # allow_heat skips it outright, but allow_heat defaults True, so it
        # is not by itself a safe gate for an unsolicited firing on an
        # ordinary suite="lcd" run. Same shape as LCD-19 above: the case
        # requires a second, default-False opt-in, ctx["lcd22_allow_heat"]
        # (`lcd_edit_heat` here), and returns NOT_RUN naming it without.
        self.ctx["lcd22_allow_heat"] = lcd_edit_heat
        # OT-E07/OT-E08 start their own firing/autotune: same shape again, a
        # default-False opt-in required in addition to allow_heat.
        self.ctx["ota_allow_heat"] = ota_allow_heat

        # Board lock (docs/audits/profile_executor_panic_2026-09-24.md
        # HP-02/HP-05): acquired here, before preflight even runs, for any
        # suite `board_lock.suite_is_mutating()` calls mutating -- a
        # read-only suite never creates the main lock file, but registers a
        # reader marker (also released below) and is refused while a live
        # mutating run holds the lock; a mutating suite is refused while a
        # live read-only reader marker is registered, closing that gap the
        # other way too.
        # Refuses immediately (never waits) if a live process already holds
        # it; a stale lock (holder pid confirmed dead) is reclaimed under
        # exclusion -- at most one of several racing reclaimers can win --
        # with a logged notice. Held for the ENTIRE run, released only in the
        # `finally` below -- including the case where the MCP client's own
        # 300 s tool timeout fires: the server keeps executing this method
        # regardless of whether the HTTP reply was ever read, so the lock
        # must track the run's real lifetime, not the reply.
        lock = board_lock.acquire(suite, tag=tag, logs_root=self.logs_root)
        # Everything after the acquire is inside the try so the lock is
        # released on EVERY exit path, including an exception from the log
        # setup below.
        try:
            # Create the run directory and runner.log immediately (ROADMAP
            # B3): a stall anywhere before the first case now leaves a
            # timestamped trail naming the last step, instead of nothing.
            run_dir = self.ctx["run_dir"]
            try:
                os.makedirs(run_dir, exist_ok=True)
                self._runner_log_path = os.path.join(run_dir, RUNNER_LOG_NAME)
            except OSError as exc:
                self._runner_log_path = None
                self.transcript.append(f"WARNING: could not create run dir {run_dir}: {exc}")
            self._runner_log(f"board lock: {'held' if lock is not None else 'not needed (read-only suite)'}")
            if lock is not None and lock.reclaimed_from is not None:
                self._log(
                    f"board lock: reclaimed stale lock from dead {lock.reclaimed_from.describe()}"
                )
            self._log(f"# bench_test run {run_id} (suite={suite}, dry_run={dry_run})")
            self._log(f"requested cases: {', '.join(requested)}")

            outcome = self._run_locked(
                suite=suite, requested=requested, run_id=run_id, started=started,
                dry_run=dry_run, allow_heat=allow_heat,
            )
        finally:
            if lock is not None:
                lock.release()
        return outcome

    def _run_start_probes(self, requested) -> None:
        """Take read-only start-of-run baselines (CaseSpec.run_start_probe).
        A failing probe leaves its key unset and never affects a verdict."""
        for cid in requested:
            probe = get_case(cid).run_start_probe
            if probe is None:
                continue
            key, fn = probe
            ok, val = _safe_call(fn, self.ctx)
            if ok and val is not None:
                self.ctx[key] = val
            else:
                self._log(f"run-start probe for {cid} yielded no baseline")

    def _run_locked(self, *, suite: str, requested: List[str], run_id: str, started: float,
                     dry_run: bool, allow_heat: bool) -> RunOutcome:
        """The actual preflight/execute/teardown/report body, run only once
        the board lock (if this suite needs one) is held. Split out of
        `run()` so the lock's `finally` wraps exactly this and nothing
        about lock acquisition itself."""
        preflight_ok, preflight_reason, board_before = self.preflight()
        self._log(f"preflight: {'OK' if preflight_ok else 'FAILED - ' + preflight_reason}")

        results: Dict[str, CaseResult] = {}
        # Read-only view for summary cases (OT-B02): the live results dict.
        self.ctx["_run_results"] = results
        # Same dict object, for alias judges (WEB-LOG-03): plan section 2 rule 7.
        self.ctx["_results"] = results
        windows.register_probes(self.ctx, requested, get_case)
        executed: List[str] = []
        if preflight_ok:
            self._run_start_probes(requested)

        estop_unverified = bool(self.ctx.get("estop_unverified", False)) if preflight_ok else False
        if estop_unverified:
            from ..capability_preflight import ESTOP_UNVERIFIED_LINE  # noqa: PLC0415
            self._log(f"WARNING: {ESTOP_UNVERIFIED_LINE}")
            # Defence in depth: the per-case opt-ins that let a non-heat-flagged
            # case start its own firing (LCD-19 and friends) are forced off.
            for k in ("allow_heat", "lcd19_allow_heat", "lcd22_allow_heat", "ota_allow_heat"):
                self.ctx[k] = False
        if not preflight_ok:
            for cid in requested:
                results[cid] = CaseResult(Verdict.NOT_RUN, reason=f"preflight failed: {preflight_reason}")
        else:
            for cid in requested:
                spec = get_case(cid)
                if spec.heat and estop_unverified:
                    results[cid] = CaseResult(Verdict.SKIP, reason="estop_unverified")
                    continue
                if spec.heat and not allow_heat:
                    results[cid] = CaseResult(Verdict.SKIP, reason=spec.heat_skip_reason or "allow_heat=False")
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

        if "preflight_stall" in board_before:
            # Preflight is read-only and the link is presumed wedged: probing
            # again would only stall a second time (and push the call past
            # the MCP client's 300 s timeout). Nothing ran, nothing to stop.
            board_after = {"teardown_skipped": "preflight stalled; no case ran"}
            self._log("teardown: skipped (preflight stalled; no case ran)")
        else:
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
            tainted=bool(self.ctx.get("_tainted")),
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
              allow_heat: bool = True, lcd_stop_heat: bool = False,
             lcd_edit_heat: bool = False, ota_allow_heat: bool = False,
              tag: Optional[str] = None, host: Optional[str] = None,
              logs_root: Optional[str] = None) -> RunOutcome:
    """Convenience entry point -- NOT what mcp_server_bench_test.py's
    bench_test_run() calls; that tool builds its own ctx directly (resolving
    `host` via `mcp_server_ota._ota_resolve_host_with_source` before it ever
    reaches `ctx`) and constructs `BenchTestRunner` itself. This function has
    no caller in this tree today; it passes `host` straight through
    UNRESOLVED (a `None` here reaches `ctx["host"]` literally, same bug
    `bench_test_run`/`ota_matrix_run` had before it was fixed) -- resolve it
    the same way before using this as a real entry point. Every OTA/
    factory-reset/sw-reset route is ROUTE_TIER_ADMIN only now (the
    AP-password HMAC scheme was retired 2026-09-29, WEB_AUTH_PLAN.md item
    2b), so no credential parameter is needed here."""
    ctx: Dict[str, Any] = {"host": host, "tag": tag}
    runner = BenchTestRunner(ctx, logs_root=logs_root)
    return runner.run(suite=suite, cases=cases, dry_run=dry_run, allow_heat=allow_heat,
                       lcd_stop_heat=lcd_stop_heat,
                       lcd_edit_heat=lcd_edit_heat, ota_allow_heat=ota_allow_heat, tag=tag)
