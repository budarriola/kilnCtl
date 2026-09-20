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
"""
from __future__ import annotations

from . import judgments as J
from .registry import CaseResult, Verdict, get_case


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
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
