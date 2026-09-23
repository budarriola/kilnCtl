"""ota_matrix_run -- ROADMAP.md M8's "a scripted run_pctools_tests-style
regression so the whole [OTA] matrix can be re-run from PcTools rather than
by hand." Suite ``ota`` in ``bench_test/registry.py`` (docs/
BENCH_TEST_SYSTEM_PLAN.md section 3.4) and its case bodies in
``bench_test/cases_ota.py`` already ARE that matrix -- this module is a
thin, safety-first front door onto ``BenchTestRunner``, the same engine
``bench_test_run()`` (mcp_server_bench_test.py) already drives for every
other suite, not a second case-running engine.

What this wrapper adds on top of the general ``bench_test_run()`` tool:

- refuses to touch the board at all unless ``confirm=True`` -- suite
  ``ota`` is the single most mutating suite this harness has (it can flash
  both processors, roll back a slot, and reset the safety link), and this
  tool exists specifically so the whole matrix can be re-run unattended,
  which makes an accidental invocation more consequential than an
  accidental ``bench_test_run(suite="smoke")``.
- ``dry_run=True`` lists suite ``ota``'s cases (fixed run order) and the
  preconditions ``BenchTestRunner.preflight()`` checks with NO board access
  whatsoever, not even a read -- distinct from
  ``bench_test_run(dry_run=True)``, which still runs the real preflight
  probes against the board before SKIPping every case.
- reuses ``BenchTestRunner.preflight()`` (which itself reuses
  ``capability_preflight.run_preflight()``, plus executor-idle,
  autotune-idle, safety-link-up and no-latched-trip checks) as the
  run-level precondition gate, and every case's own
  ``_is_idle()``/``_interlock_ok()`` (cases_ota.py) as the per-case gate
  immediately before each mutating call -- this module adds the ``confirm``
  gate and the dry-run listing on top; it does not relax or duplicate
  either of those.

The board-touching logic lives in ``_run_ota_matrix(ctx, ...)``, which takes
the same kind of injectable ``ctx`` (``srv``, ``capability_preflight_run``,
``ap_password``, ...) ``BenchTestRunner``/``cases_ota.py`` already use, so it
is unit-testable with a fake board exactly like
``test_bench_test_runner.py``/``test_bench_test_cases_ota.py`` --
this tool has never been run against real hardware, per this task's own
"no board access" constraint.
"""
from __future__ import annotations

from typing import Optional

from . import mcp_server as _srv
from .bench_test import registry as bt_registry
from .bench_test import report as bt_report
from .bench_test.runner import BenchTestRunner

#: The one suite this tool ever runs -- see this module's docstring for why
#: it does not take a `suite` parameter the way `bench_test_run` does.
_OTA_SUITE = "ota"


def _ota_preconditions_text() -> str:
    return "\n".join([
        "Preconditions checked by BenchTestRunner.preflight() before ANY case in "
        "this matrix runs (refuses the whole run, no case attempted, if any fails):",
        "  - MCP server not reporting stale code (kiln_help()/fresh vs stale)",
        "  - profile executor idle -- never OTA/reset during a firing",
        "  - autotune not active",
        "  - safety link up",
        "  - no safety trip latched (an S6a trip mid-clear is OT-B01's own job, "
        "not a run-level precondition)",
        "  - capability_preflight: board reachable, no unacknowledged crash report, "
        "no readiness-gate blocker",
        "Checked again, per case, immediately before each mutating call "
        "(cases_ota.py's _is_idle()/_interlock_ok()):",
        "  - executor still idle",
        "  - GET /api/ota/interlock reports ok:true",
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
        heat = " [heat]" if spec.heat else ""
        dep = f" (depends_on={spec.depends_on})" if spec.depends_on else ""
        lines.append(f"  {cid}: {spec.description}{heat}{dep}{impl}")
    return "\n".join(lines)


def _run_ota_matrix(ctx: dict, cases: Optional[str], tag: Optional[str],
                     allow_flash: bool, logs_root: Optional[str] = None) -> str:
    """The real, board-touching path -- only ever reached once the tool
    wrapper below has confirmed ``confirm=True``. ``ctx`` carries
    ``host``/``ap_password`` plus whatever test-only overrides (``srv``,
    ``capability_preflight_run``, ``_now``/``_sleep``, ...) a caller wants
    ``BenchTestRunner``/the case bodies to see instead of the real board --
    exactly the seam ``test_bench_test_runner.py`` and
    ``test_bench_test_cases_ota.py`` already exercise."""
    case_list = [c.strip() for c in cases.split(",") if c.strip()] if cases else None
    ctx = dict(ctx)
    ctx["allow_flash"] = allow_flash
    runner = BenchTestRunner(ctx, logs_root=logs_root)
    try:
        outcome = runner.run(suite=_OTA_SUITE, cases=case_list, dry_run=False,
                              allow_heat=True, tag=tag)
    except (KeyError, ValueError) as exc:
        return f"error: {exc}"

    lines = [f"ota_matrix_run: suite=ota run_id={outcome.run_id} exit_code={outcome.exit_code}"]
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
                    ap_password: Optional[str] = None, host: Optional[str] = None,
                    tag: Optional[str] = None, allow_flash: bool = False) -> str:
    """Run the whole OTA test matrix -- ROADMAP.md M8's "scripted
    run_pctools_tests-style regression" for suite `ota`
    (docs/BENCH_TEST_SYSTEM_PLAN.md section 3.4: OT-B01, OT-E01..12,
    OT-P01..05) -- against this board. A thin wrapper around the same
    `BenchTestRunner` engine `bench_test_run(suite="ota")` already drives
    (see that tool's docstring for the run-directory/summary.json/
    exit-code contract shared here); this tool adds two things on top for
    a matrix this mutating: a hard `confirm` gate and a zero-board-access
    dry run.

    Refuses with an `error:` line unless `confirm=True` -- this matrix can
    flash both processors, roll back a slot, and reset the safety link,
    and shipping it as one callable tool means it can be re-run
    unattended, which makes an accidental call more consequential than
    most other tools here. `dry_run=True` lists suite `ota`'s cases (fixed
    run order) and the preconditions the run would check, WITHOUT reading
    anything from the board at all; pass `confirm=True` (with
    `dry_run=False`, the default) to actually run it. `dry_run` and a
    missing `confirm` are independent checks -- the case list is visible
    with neither.

    `cases` narrows to an explicit comma-separated subset of suite `ota`'s
    ids (e.g. `"OT-E01,OT-E02"`); an id outside that suite is refused.
    `ap_password`/`host`/`tag`/`allow_flash` pass straight through to the
    same runner `bench_test_run` uses. Never flashes/resets during a
    firing or with the OTA interlock not-ok: every case already gates
    itself on that immediately before acting (cases_ota.py's
    `_is_idle()`/`_interlock_ok()`), and the run-level preflight
    (`BenchTestRunner.preflight()`, reusing
    `capability_preflight.run_preflight()`) refuses the whole run outright
    first if the executor isn't idle, autotune is active, the safety link
    is down, a trip is latched, or there is an unacknowledged crash report
    / readiness-gate blocker."""
    if dry_run:
        return _dry_run_listing(cases)
    if not confirm:
        return (
            "error: refused -- ota_matrix_run mutates the board (can flash both "
            "processors, roll back a slot, reset the safety link); pass confirm=True "
            "to run it. Pass dry_run=True first to see the case list and preconditions "
            "with no board access."
        )
    ctx = {"host": host, "ap_password": ap_password}
    return _run_ota_matrix(ctx, cases=cases, tag=tag, allow_flash=allow_flash)
