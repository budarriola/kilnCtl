#!/usr/bin/env python3
"""bd_reachability_check.py -- answers PID_EXPANSION_PLAN.md sec 3.6b's
standing pre-flight question FROM A CAPTURE, after the fact: did the field
varied between two A/B arms actually reach the control law and differ in
real ``bd_*`` telemetry, or did both arms run the identical code path (the
fuzzy-PID inert-campaign signature: ``control_mode:2`` in both presets meant
the fuzzy layer, gated on ``control_mode==3``, never ran in either arm --
commit ``8906686``, fixed in ``91c5d6d``)?

Sec 3.6b requires this proof be done LIVE, before committing kiln time to a
campaign. This module is the complementary, after-the-fact check: given two
arms' HTTP-capture ``.jsonl`` files that were made with
``run_queue.RunQueueConfig.capture_control_bd=True`` (the CLI default since
this module was added), it can confirm the SAME thing from the recorded
``"control"`` bodies -- so an inert campaign is visible from its own data,
not only from someone remembering to run the live check. A capture with no
``"control"`` key (predating this feature, or made with
``--no-capture-control-bd``) cannot be checked this way at all -- see
``ease_off_window_mult``'s captures, PID_EXPANSION_PLAN.md sec 3.6b's own
note about exactly this gap.

WHAT "DIFFERS" MEANS HERE. Per zone, per ``bd_*`` field
(``http_capture_log.BD_FIELDS``, the twelve keys off
``zone_duty_breakdown_t``), this summarizes every sample in each arm to
``(min, max, mean, n)`` and compares the two arms' summaries. The inert
signature is EXACT equality of that summary between arms -- not "close", not
"within noise": a field that never reached a code path gated by the varied
preset value produces the literal same numbers regardless of which arm ran,
because both arms executed the identical branch. A field that DID reach a
differing code path will show even a small numeric difference, since no two
independent firings produce bit-identical floats by chance (different
ambient, different tick timing, different noise). So exact equality is the
correct test for "never reached the control law differently" -- it is
deliberately NOT a "materially different" or noise-band test; that question
belongs to ``pid_ab_compare.py``, which answers a completely different
question (did the varied field's effect show up in tracking error) once
reachability is no longer in doubt.

CLI:

    python -m kilnctrl.bd_reachability_check ARM_A.jsonl ARM_B.jsonl \\
        [--field bd_kp_effective ...] [--zone N] [--json]

Exit codes: 0 = at least one checked field reached and differed in every
zone checked (REACHABLE); 1 = every checked field was bit-identical in at
least one zone (INERT -- the loud failure this module exists to catch); 2 =
one or both captures have no "control" data to check at all (refuses to
guess).
"""
from __future__ import annotations

import argparse
import dataclasses
import json
from typing import Optional, Sequence

from kilnctrl import http_capture_log as hc

#: The subset of http_capture_log.BD_FIELDS this module checks by default --
#: all twelve. A caller narrows with --field/--fields when only the term
#: actually under test in a given campaign matters (e.g. just
#: bd_kp_effective/bd_ki_effective/bd_kd_effective for a gain-fuzzing A/B).
DEFAULT_FIELDS = hc.BD_FIELDS


@dataclasses.dataclass
class ArmSummary:
    """One zone/field's (min, max, mean, n) across every sample in one arm's
    capture that carried a ``"control"`` body for that zone."""
    min: float
    max: float
    mean: float
    n: int


@dataclasses.dataclass
class FieldVerdict:
    zone: int
    field: str
    a: ArmSummary
    b: ArmSummary
    #: True iff (min, max, mean) is EXACTLY equal between arm A and arm B --
    #: the inert signature (see module docstring). False means the field
    #: reached a code path that produced at least some numeric difference.
    bit_identical: bool


class ReachabilityCheckError(ValueError):
    """Raised when a capture has no ``"control"`` data at all for a zone
    that was requested -- refusing to report REACHABLE or INERT from data
    that was never collected is the whole point of this module existing."""


def _summarize(values: Sequence[float]) -> ArmSummary:
    return ArmSummary(min=min(values), max=max(values), mean=sum(values) / len(values),
                       n=len(values))


def _collect_field_values(path: str, field: str) -> dict:
    """``{zone_index: [values, ...]}`` for ``field`` across every sample in
    ``path`` that carries a ``"control"`` body with that zone/field present.
    Samples with no ``"control"`` key (or a zone/field missing from it) are
    silently skipped -- a capture made before the run's arm applied, or a
    zone that faulted out mid-run, should not poison the summary, but an
    ENTIRELY empty result (checked by the caller) means the capture never
    had this data at all."""
    out: dict = {}
    rows = hc.parse_http_capture_jsonl(path)
    for row in rows:
        by_zone = hc.bd_fields_by_zone(row.control)
        for zone, fields in by_zone.items():
            if field not in fields:
                continue
            val = fields[field]
            if not isinstance(val, (int, float)):
                continue
            out.setdefault(zone, []).append(float(val))
    return out


def compare_bd_reachability(
    path_a: str, path_b: str,
    fields: Sequence[str] = DEFAULT_FIELDS,
    zones: Optional[Sequence[int]] = None,
) -> list[FieldVerdict]:
    """Per (zone, field) in ``fields`` (and ``zones``, or every zone either
    capture reports if not given), compare arm A's and arm B's summaries.
    Raises :class:`ReachabilityCheckError` if EITHER capture has no
    ``"control"`` data for a requested field/zone at all -- that is a
    "cannot check" state, never silently reported as INERT or REACHABLE."""
    verdicts: list[FieldVerdict] = []
    for field in fields:
        a_by_zone = _collect_field_values(path_a, field)
        b_by_zone = _collect_field_values(path_b, field)
        if not a_by_zone and not b_by_zone:
            raise ReachabilityCheckError(
                f"neither {path_a!r} nor {path_b!r} has any \"control\" data for "
                f"{field!r} -- both captures need run_queue.py's capture_control_bd=True "
                "(the CLI default; check for --no-capture-control-bd) to answer sec 3.6b's "
                "question from data instead of a live check")
        requested_zones = zones if zones is not None else sorted(set(a_by_zone) | set(b_by_zone))
        for zone in requested_zones:
            a_vals = a_by_zone.get(zone)
            b_vals = b_by_zone.get(zone)
            if not a_vals or not b_vals:
                missing = path_a if not a_vals else path_b
                raise ReachabilityCheckError(
                    f"{missing!r} has no \"control\" samples for zone {zone}, field {field!r} "
                    "-- cannot compare; re-capture with capture_control_bd=True, or narrow "
                    "--zone/--field to what was actually captured")
            a_summary = _summarize(a_vals)
            b_summary = _summarize(b_vals)
            bit_identical = (
                a_summary.min == b_summary.min
                and a_summary.max == b_summary.max
                and a_summary.mean == b_summary.mean
            )
            verdicts.append(FieldVerdict(zone=zone, field=field, a=a_summary, b=b_summary,
                                          bit_identical=bit_identical))
    return verdicts


def reachable_zones(verdicts: Sequence[FieldVerdict]) -> list:
    """Sorted zones in which AT LEAST ONE checked field differed between the
    arms -- i.e. the varied preset field demonstrably reached the control law
    *in that zone*."""
    zones = {v.zone for v in verdicts}
    return sorted(z for z in zones
                  if any(not v.bit_identical for v in verdicts if v.zone == z))


def inert_zones(verdicts: Sequence[FieldVerdict]) -> list:
    """Sorted zones in which EVERY checked field was bit-identical between
    the arms -- the inert signature, *per zone*.

    2026-09-04 -- WHY THIS EXISTS, AND WHY ``overall_reachable`` IS AN "AND"
    ACROSS ZONES RATHER THAN AN "OR". This module originally answered
    reachability with ``any(not v.bit_identical ...)`` over the flat verdict
    list: one differing (zone, field) anywhere made the whole pair
    REACHABLE. That contradicted this module's own documented exit-code
    contract ("0 = at least one checked field reached and differed in EVERY
    zone checked"), and it is unsound for the consumer that matters: the
    campaign decision rule is "same metric, same direction, >= 3 zones", so
    a pair in which the fuzzy layer demonstrably acted in zone 0 but was
    bit-identical -- provably inert -- in zones 1 and 2 would still have all
    three zones counted toward that rule. Two of the three "votes" would
    then come from zones where the treatment provably never acted: a
    confident verdict from data that cannot support it, which is the exact
    failure class this tooling exists to prevent. Reachability is therefore
    judged PER ZONE, and the consumer excludes inert zones from the decision
    rule rather than voiding or silently including them."""
    zones = {v.zone for v in verdicts}
    reach = set(reachable_zones(verdicts))
    return sorted(z for z in zones if z not in reach)


def overall_reachable(verdicts: Sequence[FieldVerdict]) -> bool:
    """True iff EVERY checked zone had at least one field differ between
    arms -- matching this module's documented exit-code contract. False
    (INERT, exit 1) when any checked zone was entirely bit-identical; see
    :func:`inert_zones` for why this is an "and" across zones. Callers that
    need the finer picture (which zones are usable) should use
    :func:`reachable_zones` / :func:`inert_zones` directly."""
    if not verdicts:
        return False
    return not inert_zones(verdicts)


def format_report(path_a: str, path_b: str, verdicts: Sequence[FieldVerdict]) -> str:
    lines = [f"bd_* reachability check: {path_a} (A) vs {path_b} (B)", ""]
    inert = [v for v in verdicts if v.bit_identical]
    differing = [v for v in verdicts if not v.bit_identical]
    for v in verdicts:
        tag = "INERT (bit-identical)" if v.bit_identical else "differs"
        lines.append(
            f"  zone {v.zone} {v.field}: A[min={v.a.min:.6g} max={v.a.max:.6g} "
            f"mean={v.a.mean:.6g} n={v.a.n}]  B[min={v.b.min:.6g} max={v.b.max:.6g} "
            f"mean={v.b.mean:.6g} n={v.b.n}]  -> {tag}")
    lines.append("")
    inert_z = inert_zones(verdicts)
    reach_z = reachable_zones(verdicts)
    if differing and inert_z:
        lines.append(
            f"PARTIALLY INERT: zone(s) {inert_z} were EXACTLY bit-identical across every "
            f"checked field, while zone(s) {reach_z} differed. The varied field reached the "
            "control law in some zones but provably NOT in "
            f"{inert_z} -- those zones must be EXCLUDED from any >=3-zone decision rule; "
            "counting them would draw a conclusion from zones the treatment never acted on.")
    if not differing:
        lines.append(
            "VERDICT: INERT -- every checked (zone, field) was EXACTLY bit-identical between "
            "arms. This is the fuzzy-PID inert-campaign signature (commit 8906686): the "
            "varied preset field never reached the control law in either arm. DO NOT trust "
            "any A/B conclusion drawn from this pair until the wiring is fixed and re-fired.")
    else:
        lines.append(
            f"VERDICT: REACHABLE -- {len(differing)}/{len(verdicts)} checked (zone, field) "
            f"combinations differed between arms ({len(inert)} bit-identical). The varied "
            "field reached the control law in real telemetry.")
    return "\n".join(lines)


def _jsonable(verdicts: Sequence[FieldVerdict]) -> list:
    return [dataclasses.asdict(v) for v in verdicts]


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-bd-reachability-check",
        description="PID_EXPANSION_PLAN.md sec 3.6b: confirm from two arms' HTTP captures "
                    "that the varied field actually reached the control law and differed in "
                    "real bd_* telemetry -- the after-the-fact form of the standing "
                    "pre-flight check.")
    parser.add_argument("path_a")
    parser.add_argument("path_b")
    parser.add_argument("--field", dest="fields", action="append", default=None,
                         metavar="BD_FIELD",
                         help="check only this bd_* field (repeatable). Default: all twelve.")
    parser.add_argument("--zone", dest="zones", type=int, action="append", default=None,
                         help="check only this zone index (repeatable). Default: every zone "
                              "either capture reports.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    fields = tuple(args.fields) if args.fields else DEFAULT_FIELDS
    try:
        verdicts = compare_bd_reachability(args.path_a, args.path_b, fields=fields,
                                            zones=args.zones)
    except ReachabilityCheckError as exc:
        print(f"error: {exc}")
        return 2

    if args.json:
        print(json.dumps({
            "path_a": args.path_a, "path_b": args.path_b,
            "reachable": overall_reachable(verdicts),
            "verdicts": _jsonable(verdicts),
        }, indent=2))
    else:
        print(format_report(args.path_a, args.path_b, verdicts))

    return 0 if overall_reachable(verdicts) else 1


if __name__ == "__main__":
    raise SystemExit(main())
