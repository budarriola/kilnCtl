"""ota_matrix_run -- ROADMAP.md M8's "a scripted run_pctools_tests-style
regression so the whole [OTA] matrix can be re-run from PcTools rather than
by hand." Suite ``ota`` in ``bench_test/registry.py`` (docs/
BENCH_TEST_SYSTEM_PLAN.md section 3.4) and its case bodies in
``bench_test/cases_ota.py`` already ARE that matrix -- this module is a
thin, safety-first front door onto ``BenchTestRunner``, the same engine
``bench_test_run()`` (mcp_server_bench_test.py) already drives for every
other suite, not a second case-running engine.

**Without image paths, only OT-B01 (the dual-reflash S6a handshake trip)
can actually execute** -- every OT-E*/OT-P* case reads its own image path
out of ctx (``ota_image_path``, ``ota_corrupt_image_path``, ...,
``cases_ota.py``) and SKIPs when it is unset, exactly like calling
``bench_test_run(suite="ota")`` with none of those set today. This tool
does not manufacture images; it passes through whatever paths the caller
gives it.

What this wrapper adds on top of the general ``bench_test_run()`` tool:

- refuses to touch the board at all unless ``confirm is True`` (not merely
  truthy -- a stray string or int must never slip past this gate the way
  `mcp_server_actions.py`'s :97-133 confirm-bug class shows a bare
  ``if not confirm`` can) -- suite ``ota`` is the single most mutating
  suite this harness has (it can flash both processors, roll back a slot,
  and reset the safety link), and this tool exists specifically so the
  whole matrix can be re-run unattended, which makes an accidental
  invocation more consequential than an accidental
  ``bench_test_run(suite="smoke")``.
- ``dry_run=True`` lists suite ``ota``'s cases (fixed run order) and the
  preconditions this tool checks with NO board access whatsoever, not even
  a read -- distinct from ``bench_test_run(dry_run=True)``, which still
  runs the real preflight probes against the board before SKIPping every
  case.
- a fail-closed run-level precondition gate, checked BEFORE
  ``BenchTestRunner`` is even constructed: this reuses
  ``mcp_server_coordinated_gpio_test._gpio_test_preflight()`` (the same
  ARMED/link-up/profile-idle/OTA-interlock probe `coordinated_gpio_test`
  already refuses on, tri-state so an unreadable status refuses rather than
  passing) plus an explicit `capability_preflight` read (unreadable, or a
  present crash report, is also a refusal here) -- because
  ``BenchTestRunner.preflight()`` on its own (runner.py `_safe_call`) folds
  every probe exception into "could not determine" and only treats the
  *executor* being in the literal `"running"` state as busy, never
  `"paused"`, and never reads ARMED or the interlock at the run level at
  all. This module's gate is *in addition to* that one, not a replacement
  for it -- `BenchTestRunner.preflight()` still runs too.
- every case's own ``_is_idle()``/``_interlock_ok()`` (cases_ota.py) still
  gates each mutating call immediately before it acts, with one deliberate,
  named exception: OT-E07/OT-E08 (`cases_ota.py`'s
  `_case_update_refused_during_state()`) intentionally push *during* a
  firing/autotune run, because proving the push is refused is the entire
  point of those two cases -- they are only reachable at all when
  ``allow_heat=True`` is passed (default ``False``), the same
  `BenchTestRunner` knob `bench_test_run` already exposes, mirroring how
  `run_all_checks.ps1`'s heat-bearing checks are opt-in. OT-B01 (the
  dual-reflash trip) checks the executor is idle and clears any latched
  trip but does **not** itself check the OTA interlock -- this tool's own
  run-level gate above covers that gap.

The board-touching logic lives in ``_run_ota_matrix(ctx, ...)``, which takes
the same kind of injectable ``ctx`` (``srv``, ``capability_preflight_run``,
``gpio_test_preflight_fn``, the ``ota_*_path``/``ota_*_commit`` image
parameters, ...) ``BenchTestRunner``/``cases_ota.py`` already use, so it is
unit-testable with a fake board exactly like
``test_bench_test_runner.py``/``test_bench_test_cases_ota.py`` -- this tool
has never been run against real hardware, per this task's own "no board
access" constraint.
"""
from __future__ import annotations

from typing import Optional

from . import mcp_server as _srv
from . import mcp_server_coordinated_gpio_test as _gpio_tool
from . import mcp_server_ota as _ota_tool
from .bench_test import board_lock as bt_board_lock
from .bench_test import registry as bt_registry
from .bench_test import report as bt_report
from .bench_test.runner import BenchTestRunner

#: The one suite this tool ever runs -- see this module's docstring for why
#: it does not take a `suite` parameter the way `bench_test_run` does.
_OTA_SUITE = "ota"

#: ctx keys each image-driven case (`cases_ota.py`) reads its input from --
#: this tool's `ota_matrix_run()` parameters map straight onto these, one
#: per name, so a caller who wants more than OT-B01 to actually execute has
#: to supply the same paths `bench_test_run(suite="ota", ...)` would need.
_IMAGE_CTX_KEYS = (
    "ota_image_path", "ota_corrupt_image_path", "ota_truncated_image_path",
    "ota_wrong_build_image_path", "ota_image_build", "ota_pico_image_path",
    "ota_pico_image_commit", "ota_pico_corrupt_image_path",
)


def _ota_preconditions_text() -> str:
    return "\n".join([
        "Run-level preconditions checked by this tool BEFORE BenchTestRunner is even "
        "constructed (refuses the whole run, no case attempted, if any fails; unreadable "
        "counts as failed, never as passed):",
        "  - safety relay confirmed NOT ARMED (SafetyFlag.ENABLED clear)",
        "  - safety link confirmed up",
        "  - profile executor confirmed idle (running OR paused refuses)",
        "  - GET /api/ota/interlock confirmed ok:true",
        "  - capability_preflight: board reachable, no unacknowledged crash report, "
        "no readiness-gate blocker (an unreadable capability_preflight also refuses)",
        "Then BenchTestRunner.preflight() runs its own checks on top (MCP staleness, "
        "executor/autotune idle, safety link, latched trip, capability_preflight again).",
        "Checked again, per case, immediately before each mutating call "
        "(cases_ota.py's _is_idle()/_interlock_ok()) -- EXCEPT OT-E07/OT-E08, which "
        "deliberately push during a firing/autotune run to prove the push is refused; "
        "those two only run at all when allow_heat=True (default False). OT-B01 checks "
        "executor-idle and clears a latched trip but does not itself check the OTA "
        "interlock -- covered by this tool's run-level gate above, not by OT-B01 itself.",
        "Without ota_image_path/ota_pico_image_path (and friends) set, every OT-E*/OT-P* "
        "case SKIPs for lack of an image -- only OT-B01 actually executes.",
    ])


def _dry_run_listing(cases: Optional[str]) -> str:
    """No board access at all -- pure registry data."""
    ota_ids = bt_registry.suite_case_ids(_OTA_SUITE)
    requested = ota_ids
    if cases:
        wanted = {c.strip() for c in cases.split(",") if c.strip()}
        unknown = sorted(wanted - set(ota_ids))
        if unknown:
            return f"error: case id(s) not in suite {_OTA_SUITE!r}: {', '.join(unknown)}"
        requested = [c for c in ota_ids if c in wanted]

    lines = [
        f"ota_matrix_run: dry_run -- {len(requested)} case(s) in suite {_OTA_SUITE!r}, no board access",
        "",
        _ota_preconditions_text(),
        "",
        "cases (fixed run order):",
    ]
    for cid in requested:
        spec = bt_registry.get_case(cid)
        impl = "" if spec.judge is not None else " [not implemented]"
        heat = " [heat -- needs allow_heat=True]" if spec.heat else ""
        dep = f" (depends_on={spec.depends_on})" if spec.depends_on else ""
        lines.append(f"  {cid}: {spec.description}{heat}{dep}{impl}")
    return "\n".join(lines)


def _ota_refusal_reasons(pf) -> "list[str]":
    """Same tri-state fields `GpioTestPreflight.refusal_reasons()` checks,
    reworded for an OTA run instead of the GPIO-detach test that dataclass
    was originally written for -- its own text ("refusing to detach
    GPIO4/5/10 from firmware...") is misleading here."""
    reasons = []
    if pf.safety_armed is not False:
        reasons.append(f"safety relay is ARMED or its state could not be confirmed "
                        f"(safety_armed={pf.safety_armed!r}) -- refusing to run an OTA "
                        f"update while the safety chain could be live")
    if pf.profile_running_or_paused is not False:
        reasons.append(f"a profile is running or paused, or its state could not be "
                        f"confirmed (profile_state={pf.profile_state_name!r}) -- an OTA "
                        f"update must not run during a firing")
    if pf.ota_interlock_ok is not True:
        reasons.append(f"OTA interlock is not idle or could not be confirmed: "
                        f"{pf.ota_interlock_reason}")
    if pf.link_up is not True:
        reasons.append(f"safety link was not confirmed up before the run "
                        f"(link_up={pf.link_up!r})")
    return reasons


def _run_level_preflight(ctx: dict, host: Optional[str]) -> Optional[str]:
    """Fail-closed gate run BEFORE `BenchTestRunner` is constructed. Returns
    a refusal reason string, or `None` if every check passed. Every probe
    is read through `ctx` so tests can inject a fake board without ever
    reaching a real one -- production code (the `ota_matrix_run` tool
    below) leaves both keys unset and gets the real checks. `host` is
    resolved once (explicit host, else the board's STA IP, else the
    fallback-AP address -- `coordinated_gpio_test`'s own resolution order)
    and that same resolved value is passed to both probes below, so a
    LAN-only board with no explicit `host` does not get a spurious refusal
    from capability_preflight defaulting to the AP address while the gpio
    preflight resolved the real STA IP."""
    resolve_host_fn = ctx.get("resolve_host_fn", _gpio_tool._gpio_test_resolve_host)
    resolved_host = resolve_host_fn(host)

    gpio_preflight_fn = ctx.get("gpio_test_preflight_fn", _gpio_tool._gpio_test_preflight)
    try:
        pf = gpio_preflight_fn(resolved_host)
    except Exception as exc:  # noqa: BLE001 -- unreadable must refuse, not pass
        return f"could not read run-level preconditions (ARMED/link/idle/interlock): {exc}"
    reasons = _ota_refusal_reasons(pf)
    if reasons:
        return "; ".join(reasons)

    from . import capability_preflight  # noqa: PLC0415

    cp_run = ctx.get("capability_preflight_run", capability_preflight.run_preflight)
    try:
        cp_report = cp_run({}, resolved_host)
    except Exception as exc:  # noqa: BLE001 -- unreadable must refuse, not pass
        return f"could not read capability_preflight: {exc}"
    if not cp_report.ok:
        if cp_report.board.crash_unacknowledged:
            return f"unacknowledged crash report present: {cp_report.board.crash_summary}"
        if cp_report.board.readiness_blocked:
            names = ", ".join(k for k, _l, _d in cp_report.board.readiness_blocked)
            return f"readiness gate blocks: {names}"
        if not cp_report.board.reachable:
            return f"capability_preflight: board unreachable: {cp_report.board.error}"
        return "capability_preflight reports not ok"
    return None


def _run_ota_matrix(ctx: dict, cases: Optional[str], tag: Optional[str],
                     allow_heat: bool, logs_root: Optional[str] = None) -> str:
    """The real, board-touching path -- only ever reached once the tool
    wrapper below has confirmed ``confirm is True``. ``ctx`` carries
    ``host``/the ``ota_*`` image keys plus whatever
    test-only overrides (``srv``, ``capability_preflight_run``,
    ``gpio_test_preflight_fn``, ...) a caller wants
    ``BenchTestRunner``/the case bodies to see instead of the real board --
    exactly the seam ``test_bench_test_runner.py`` and
    ``test_bench_test_cases_ota.py`` already exercise."""
    host = ctx.get("host")
    refusal = _run_level_preflight(ctx, host)
    if refusal is not None:
        return f"error: refused -- run-level precondition failed: {refusal}"

    case_list = [c.strip() for c in cases.split(",") if c.strip()] if cases else None
    ctx = dict(ctx)
    runner = BenchTestRunner(ctx, logs_root=logs_root)
    try:
        outcome = runner.run(suite=_OTA_SUITE, cases=case_list, dry_run=False,
                              allow_heat=allow_heat, tag=tag)
    except (KeyError, ValueError) as exc:
        return f"error: {exc}"
    except bt_board_lock.BoardLockHeld as exc:
        # Fail closed, before preflight ever ran (docs/audits/
        # profile_executor_panic_2026-09-24.md HP-02/HP-05): another
        # mutating run (this suite included) already owns the board.
        return f"error: refused -- {exc}"

    lines = [f"ota_matrix_run: suite=ota run_id={outcome.run_id} exit_code={outcome.exit_code}",
             f"host: {ctx.get('host')} ({ctx.get('host_source', 'unknown')})"]
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
def ota_matrix_run(confirm: bool = False, dry_run: bool = False, cases: Optional[str] = None,
                    host: Optional[str] = None,
                    tag: Optional[str] = None, allow_heat: bool = False,
                    ota_image_path: Optional[str] = None, ota_corrupt_image_path: Optional[str] = None,
                    ota_truncated_image_path: Optional[str] = None,
                    ota_wrong_build_image_path: Optional[str] = None,
                    ota_image_build: Optional[str] = None,
                    ota_pico_image_path: Optional[str] = None,
                    ota_pico_image_commit: Optional[str] = None,
                    ota_pico_corrupt_image_path: Optional[str] = None) -> str:
    """Run the OTA test matrix -- ROADMAP.md M8's "scripted
    run_pctools_tests-style regression" for suite `ota`
    (docs/BENCH_TEST_SYSTEM_PLAN.md section 3.4: OT-B01, OT-E01..12,
    OT-P01..05) -- against this board. A thin wrapper around the same
    `BenchTestRunner` engine `bench_test_run(suite="ota")` already drives
    (see that tool's docstring for the run-directory/summary.json/
    exit-code contract shared here); this tool adds a hard `confirm` gate,
    a zero-board-access dry run, and a fail-closed run-level precondition
    check on top.

    **Without any `ota_*` image parameter set, only OT-B01 (the dual-reflash
    S6a handshake trip) actually executes -- every other case reads its own
    image path out of ctx and SKIPs for lack of one**, same as calling
    `bench_test_run(suite="ota")` with none set. Pass the relevant
    `ota_image_path`/`ota_corrupt_image_path`/`ota_truncated_image_path`/
    `ota_wrong_build_image_path`/`ota_image_build` (ESP cases) and/or
    `ota_pico_image_path`/`ota_pico_image_commit`/
    `ota_pico_corrupt_image_path` (Pico cases) to let more of the matrix run;
    `cases` can narrow to just the ones you have images for.

    Refuses with an `error:` line unless `confirm is True` exactly (not
    merely truthy -- a string or int does not count) -- this matrix can
    flash both processors, roll back a slot, and reset the safety link, and
    shipping it as one callable tool means it can be re-run unattended,
    which makes an accidental call more consequential than most other tools
    here. `dry_run=True` lists suite `ota`'s cases (fixed run order) and the
    preconditions the run would check, WITHOUT reading anything from the
    board at all; pass `confirm=True` (with `dry_run=False`, the default) to
    actually run it. `dry_run` and a missing `confirm` are independent
    checks -- the case list is visible with neither.

    `allow_heat=True` is required for OT-E07/OT-E08 to run at all -- they
    deliberately push an update during a firing/autotune run specifically to
    prove the push is refused, so they are opt-in the same way
    `bench_test_run`'s heat-bearing cases are; default `False` skips them.

    `cases` narrows to an explicit comma-separated subset of suite `ota`'s
    ids (e.g. `"OT-E01,OT-E02"`); an id outside that suite is refused, and a
    subset cannot be used to reach a case in another suite (e.g. `"FL-10"`).
    Every route this matrix drives is ROUTE_TIER_ADMIN only, on or off --
    the AP-password HMAC challenge/response scheme they used to ALSO require
    was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b), so no separate
    credential is needed or accepted here any more.
    `host=None` (the default) is resolved before it ever reaches `ctx` --
    explicit host wins, else the board's STA IP, else the fallback-AP
    address (`mcp_server_ota._ota_resolve_host_with_source`), same as
    `bench_test_run` and `get_heap_status`. `tag` passes straight through to
    the same runner `bench_test_run` uses. The report always names the
    resolved host and how it was resolved (`explicit`/`STA IP`/`default`),
    since the `default` fallback can be a stale cached address rather than
    the board actually on the bench -- worth knowing before a call that can
    flash both processors. The resolver is never called when
    `dry_run=True`: a dry run makes no board contact at all.

    Before `BenchTestRunner` is even constructed, this tool runs its own
    fail-closed run-level gate (`_run_level_preflight`): safety relay
    confirmed NOT ARMED, safety link confirmed up, profile executor
    confirmed idle (running OR paused refuses), the OTA interlock confirmed
    ok, and `capability_preflight` confirmed reachable with no unacknowledged
    crash report or readiness-gate blocker -- any of these being unreadable
    refuses the whole run rather than proceeding. `BenchTestRunner.preflight()`
    still runs its own checks after that. Per-case, `cases_ota.py`'s
    `_is_idle()`/`_interlock_ok()` gate every mutating call immediately
    before it acts, except OT-E07/OT-E08 as described above; OT-B01 does not
    itself check the OTA interlock, which is why this tool's run-level gate
    checks it independently."""
    if dry_run:
        return _dry_run_listing(cases)
    if confirm is not True:
        return (
            "error: refused -- ota_matrix_run mutates the board (can flash both "
            "processors, roll back a slot, reset the safety link); pass confirm=True "
            "(exactly, not merely truthy) to run it. Pass dry_run=True first to see the "
            "case list and preconditions with no board access."
        )
    resolved_host, host_source = _ota_tool._ota_resolve_host_with_source(host)
    if not resolved_host:
        return "error: could not resolve a board host (no explicit host, no STA IP, no AP default)"
    ctx = {
        "host": resolved_host,
        "host_source": host_source,
        "ota_image_path": ota_image_path,
        "ota_corrupt_image_path": ota_corrupt_image_path,
        "ota_truncated_image_path": ota_truncated_image_path,
        "ota_wrong_build_image_path": ota_wrong_build_image_path,
        "ota_image_build": ota_image_build,
        "ota_pico_image_path": ota_pico_image_path,
        "ota_pico_image_commit": ota_pico_image_commit,
        "ota_pico_corrupt_image_path": ota_pico_corrupt_image_path,
    }
    return _run_ota_matrix(ctx, cases=cases, tag=tag, allow_heat=allow_heat)
