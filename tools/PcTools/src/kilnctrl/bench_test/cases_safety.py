"""SP-03 / SP-06 -- safety observers that read during another case's heat
window rather than originating their own (plan doc section 3.9, Wave 1b).

SP-03 "uses HP-02" and SP-06 "uses HP" (per the plan doc's table): both read
`cases_heat._hp_run`'s stashed-in-`ctx` before/after data
(`ctx["_hp02"]`/`ctx["_hp01"]`) rather than driving a firing of their own --
that keeps this module read-only and lets a run that only requests the
`safety` suite still report NOT_RUN with a clear reason instead of silently
starting its own heat. `registry.py` marks both `depends_on` the case they
observe so the runner's own NOT_RUN-on-unmet-dependency handling applies
when that case is requested but fails; this module's own guard covers the
case where the observed case was never requested at all.
Wave 3b adds SP-08 (E-stop press) and SP-09 (link-loss): both are
operator-only (registry.py's `operator_only=True`) and gated by
`operator.require_attended()` -- an unattended run SKIPs them with reason
"requires --attended" rather than hanging or FAILing (plan section 6). When
attended, each asks the operator to perform the physical action, reads the
resulting trip_reason off `safety_get_status()`'s text report (same regex
`cases_smoke._case_sp02` already uses), clears it via `safety_clear_trip()`,
and confirms the clear via a second status read --
`judgments.judge_operator_trip` does the actual pass/fail reasoning, kept
pure and unit-testable the same way as every other judge function here.
"""
from __future__ import annotations

import re

from . import judgments as J
from . import operator as OP
from .cases_smoke import _srv
from .registry import CaseResult, Verdict, get_case

#: SP-08 expects S7 (E-stop), SP-09 expects S6b (link-dead) -- the actual
#: enum values from firmware/SaftyFW/src/safety_guards.h (SAFETY_TRIP_ESTOP=8,
#: SAFETY_TRIP_LINK_DEAD=7). CLAUDE.md warns bit position is NOT the guard
#: number past S3, so these are named constants, never re-derived from a mask.
_SP08_EXPECTED_TRIP_REASON = 8  # SAFETY_TRIP_ESTOP (S7)
_SP09_EXPECTED_TRIP_REASON = 7  # SAFETY_TRIP_LINK_DEAD (S6b)


def _read_trip_reason(status_text: str) -> "int | None":
    m = re.search(r"trip_reason[:=]\s*(\d+)", status_text)
    return int(m.group(1)) if m else None


def _operator_trip_case(ctx: dict, question: str, expected_reason: int, timeout_s: float) -> CaseResult:
    """Shared body for SP-08/SP-09: gate on --attended, ask the operator to
    perform the action, read the trip, clear it, confirm the clear."""
    skip = OP.require_attended(ctx)
    if skip is not None:
        return skip

    answered = OP.ask_operator(ctx, question, timeout_s=timeout_s)
    if not answered:
        return CaseResult(
            Verdict.FAIL,
            reason="operator did not confirm the action was performed (answered no/timed out)",
            observed={"operator_answer": answered},
        )

    srv = _srv(ctx)
    status_text = srv.safety_get_status()
    trip_reason = _read_trip_reason(status_text)

    cleared_after: "bool | None" = None
    if trip_reason == expected_reason:
        srv.safety_clear_trip()
        after_text = srv.safety_get_status()
        after_reason = _read_trip_reason(after_text)
        if after_reason is not None:
            cleared_after = after_reason == 0

    return J.judge_operator_trip(trip_reason, expected_reason, cleared_after)


def _case_sp08(ctx: dict) -> CaseResult:
    return _operator_trip_case(
        ctx,
        "Press the E-stop now, wait a couple seconds, then release it. Confirm when done.",
        _SP08_EXPECTED_TRIP_REASON,
        timeout_s=120.0,
    )


def _case_sp09(ctx: dict) -> CaseResult:
    return _operator_trip_case(
        ctx,
        "Pull the safety link cable now, wait for the trip, then reconnect it. Confirm when done.",
        _SP09_EXPECTED_TRIP_REASON,
        timeout_s=180.0,
    )


def _case_sp03(ctx: dict) -> CaseResult:
    hp02 = ctx.get("_hp02")
    if not hp02:
        return CaseResult(Verdict.NOT_RUN, reason="HP-02 did not run in this session")
    return J.judge_link_stats_delta(hp02.get("link_stats_before", {}), hp02.get("link_stats_after", {}))


def _case_sp06(ctx: dict) -> CaseResult:
    hp01 = ctx.get("_hp01")
    if not hp01:
        return CaseResult(Verdict.NOT_RUN, reason="HP-01 did not run in this session")
    return J.judge_relay_energized(hp01.get("energized_samples", []))


def _case_sp04(ctx: dict) -> CaseResult:
    """SP-04 "uses OT-B01's S6a" (plan doc section 3.9): a pure observer of
    OT-B01's already-collected trip/clear data, same shape as SP-03/SP-06 --
    it never independently re-triggers or re-clears a trip."""
    otb01 = ctx.get("_otb01")
    if not otb01:
        return CaseResult(Verdict.NOT_RUN, reason="OT-B01 did not run in this session")
    return J.judge_dual_reset_trip(
        otb01.get("link_up"), otb01.get("trip_reason"), otb01.get("trip_mask"),
        otb01.get("clear_ok"), otb01.get("readiness_trip_ok"),
    )


_CASE_FUNCS = {
    "SP-03": _case_sp03,
    "SP-06": _case_sp06,
    "SP-04": _case_sp04,
    "SP-08": _case_sp08,
    "SP-09": _case_sp09,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
