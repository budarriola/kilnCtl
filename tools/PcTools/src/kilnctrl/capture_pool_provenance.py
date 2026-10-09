#!/usr/bin/env python3
"""capture_pool_provenance.py -- general analysis-time guard: before pooling
N HTTP-capture JSONL files (``run_queue.py``'s ``{"t","exec","status",...}``
poll format) into one analysis, check whether the pool agrees on the
REACHABILITY of a config gate field (see ``gate_fields.py``), and refuse (or
report) loudly when it does not.

THE INCIDENT THIS EXISTS FOR: an ad hoc analysis pooled 28 captures taken at
``control_mode: 2`` (plain PID) with a single capture taken at
``control_mode: 3`` (PID_FUZZY, the only mode ``pid_fuzzy_adjust()`` ever
runs under) and reported a 37,008-sample conclusion about the fuzzy layer
whose real n was 2178 -- see ``gate_fields.py``'s docstring and
``fuzzy_band_probe.py``'s. The information needed (``control_mode``, present
in every capture line) was never checked before pooling.

WHY THIS IS SEPARATE FROM ``fuzzy_band_probe.py`` AND ``bd_reachability_
check.py``, RATHER THAN A THIRD REIMPLEMENTATION OF THE SAME CHECK.
``fuzzy_band_probe.py`` already refuses to pool ``control_mode != 3`` samples
into ITS OWN membership-band math, per file, as a byproduct of doing that
analysis -- it is the right tool for a band question and this module does
not touch its math or its usable-sample accounting. ``bd_reachability_
check.py`` answers a different question (did a field reach the control law
and produce numeric daylight between exactly TWO arms) from ``"control"``
telemetry, not from the gate field itself. Neither is shaped for "a NEW
analysis script is about to pool an arbitrary-N list of capture files and
needs to know, for ANY gate field in ``gate_fields.GATE_FIELDS`` (not only
``control_mode``), whether the pool is internally consistent" -- that
general form is what this module provides, so the next gated feature (not
just the fuzzy layer) gets this check for free instead of a fourth ad hoc
script.

WHAT THIS CHECKS. For a named gate field with a ``capture_json_path``
(currently just ``control_mode`` -- see ``gate_fields.py`` for why
``ct_installed``/``ramp_assist_enabled`` cannot be checked this way, they
are config-only and never appear per-sample), every file's every zone is
read from ``exec.zones[i].<field>`` and classified reachable/unreachable per
:meth:`gate_fields.GateField.reachable_when`. A pool is flagged when, for a
given zone, SOME files show the gate reachable there and OTHERS do not --
the literal 28-mode2+1-mode3 shape. A pool that is uniformly reachable, or
uniformly unreachable, is internally CONSISTENT (not flagged) even though
"uniformly unreachable" is its own, different, problem (a fully inert
campaign) -- that case is reported separately via ``uniformly_unreachable``
and left to the caller to decide whether to treat it as fatal, since a
caller checking ``ct_installed``-shaped gates by hand might legitimately
pool nothing-but-off captures on purpose (e.g. studying behaviour WITHOUT
the feature).

CLI:

    python -m kilnctrl.capture_pool_provenance FIELD FILE [FILE ...] \\
        [--zone N] [--json]

Exit codes: 0 = pool consistent (uniformly reachable, or uniformly
unreachable, or empty pool); 1 = pool MIXES reachable and unreachable files
for at least one zone (the refusal case); 2 = a file could not be checked at
all (no data, or the field has no capture_json_path).
"""
from __future__ import annotations

import argparse
import dataclasses
import json
from typing import Optional, Sequence

from kilnctrl import gate_fields as gf
from kilnctrl.jsonl_util import iter_jsonl


class PoolProvenanceError(ValueError):
    """A file in the pool could not be checked for this gate at all -- no
    data for the field, or the gate has no per-sample capture representation.
    Distinct from :func:`check_pool_gate_consistency` finding a genuine
    mismatch: this means the question could not be answered, not that the
    answer was "inconsistent"."""


@dataclasses.dataclass
class FileGateSummary:
    path: str
    #: {zone_index: {values seen for the gate field in this file}}
    values_seen: "dict[int, set]"
    reachable_zones: "set[int]"
    unreachable_zones: "set[int]"


def _read_zone_field_values(path: str, field_name: str) -> "dict[int, set]":
    """{zone_index: {values, ...}} for exec.zones[i][field_name] across
    every line in path that has it. Mirrors fuzzy_band_probe.iter_zone_ticks'
    tolerance for malformed/short lines (skipped, not raised)."""
    out: "dict[int, set]" = {}
    for obj in iter_jsonl(path, on_error="skip"):
        if not isinstance(obj, dict):
            continue
        exec_body = obj.get("exec")
        if not isinstance(exec_body, dict):
            continue
        zones = exec_body.get("zones")
        if not isinstance(zones, list):
            continue
        for z in zones:
            if not isinstance(z, dict) or "zone" not in z or field_name not in z:
                continue
            try:
                zi = int(z["zone"])
            except (TypeError, ValueError):
                continue
            out.setdefault(zi, set()).add(z[field_name])
    return out


def summarize_file_gate(path: str, gate: gf.GateField) -> FileGateSummary:
    """Classify every zone ``path`` reports for ``gate`` as reachable or
    unreachable there (a zone can show more than one value across a file's
    lifetime -- e.g. a preset applied mid-capture -- so a zone counts as
    reachable if ANY of its recorded values satisfy ``reachable_when``, and
    unreachable only if NONE do). Raises :class:`PoolProvenanceError` if the
    gate has no per-sample representation, or if this file records the
    field nowhere at all."""
    if gate.capture_json_path is None:
        raise PoolProvenanceError(
            f"gate field {gate.name!r} has no capture_json_path -- it is config/"
            "commissioning-only (see gate_fields.py) and never appears in per-sample "
            "HTTP-capture telemetry, so pool provenance cannot be checked this way for it. "
            "Compare the config/preset snapshots used for each capture directly instead.")
    per_zone = _read_zone_field_values(path, gate.name)
    if not per_zone:
        raise PoolProvenanceError(
            f"{path!r} has no {gate.name!r} field in any exec.zones[] sample -- cannot judge "
            "reachability from data that was never recorded (an older capture format, or the "
            "wrong field name).")
    reachable = {zi for zi, vals in per_zone.items() if any(gate.reachable_when(v) for v in vals)}
    unreachable = {zi for zi in per_zone if zi not in reachable}
    return FileGateSummary(path=path, values_seen=per_zone, reachable_zones=reachable,
                            unreachable_zones=unreachable)


def check_pool_gate_consistency(
    paths: Sequence[str], gate_name: str, zones: Optional[Sequence[int]] = None,
) -> "list[str]":
    """The core check. Looks ``gate_name`` up in :data:`gate_fields.
    GATE_FIELDS`, summarizes every file in ``paths``, and returns a list of
    human-readable problem strings -- one per zone where the pool MIXES
    files in which the gate was reachable with files in which it was not.
    Returns ``[]`` when every checked zone agrees across the whole pool
    (all reachable, or all unreachable -- see module docstring for why the
    latter is not itself flagged here).

    Raises :class:`PoolProvenanceError` (not returned as a problem string)
    when the gate name is unknown, or any file cannot be summarized at all
    -- those are "cannot answer", never silently folded into "consistent"."""
    gate = gf.find_gate(gate_name)
    if gate is None:
        known = ", ".join(g.name for g in gf.GATE_FIELDS)
        raise PoolProvenanceError(
            f"no such gate field {gate_name!r} in gate_fields.GATE_FIELDS (known: {known})")
    if not paths:
        return []
    summaries = [summarize_file_gate(p, gate) for p in paths]

    all_zones: "set[int]" = set()
    for s in summaries:
        all_zones |= set(s.values_seen)
    if zones is not None:
        all_zones &= set(zones)

    problems: "list[str]" = []
    for zi in sorted(all_zones):
        reachable_files = [s.path for s in summaries if zi in s.reachable_zones]
        unreachable_files = [s.path for s in summaries if zi in s.unreachable_zones]
        if reachable_files and unreachable_files:
            problems.append(
                f"zone {zi}: pool mixes {gate.name} REACHABLE ({gate.reachable_value_desc}) "
                f"file(s) {reachable_files} with UNREACHABLE file(s) {unreachable_files} -- "
                f"{gate.feature} ran in some pooled files and not others. This is the "
                "28-mode2+1-mode3 incident's exact shape (gate_fields.py docstring): pooling "
                "these files together would attribute samples from files where the feature "
                "never ran to a conclusion about the feature. Split the pool by reachability, "
                "or drop the unreachable files, before analyzing."
            )
    return problems


def uniformly_unreachable_zones(
    paths: Sequence[str], gate_name: str, zones: Optional[Sequence[int]] = None,
) -> "list[int]":
    """Zones where EVERY file in the pool shows the gate unreachable --
    a fully inert pool for that zone. Not a "mismatch" (the pool agrees with
    itself), but worth a caller's separate attention: see module docstring."""
    gate = gf.find_gate(gate_name)
    if gate is None or not paths:
        return []
    summaries = [summarize_file_gate(p, gate) for p in paths]
    all_zones: "set[int]" = set()
    for s in summaries:
        all_zones |= set(s.values_seen)
    if zones is not None:
        all_zones &= set(zones)
    return sorted(zi for zi in all_zones if all(zi not in s.reachable_zones for s in summaries))


def assert_pool_gate_consistent(
    paths: Sequence[str], gate_name: str, zones: Optional[Sequence[int]] = None,
) -> None:
    """Fail-fast wrapper: raises :class:`PoolProvenanceError` with every
    problem string joined, if :func:`check_pool_gate_consistency` finds any.
    The one-line form an analysis script's pooling step should call before
    doing anything with the files it just globbed."""
    problems = check_pool_gate_consistency(paths, gate_name, zones=zones)
    if problems:
        raise PoolProvenanceError(
            f"pool of {len(paths)} capture(s) disagrees on {gate_name!r} reachability:\n  "
            + "\n  ".join(problems)
        )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-capture-pool-provenance",
        description="Refuse (or flag) a pool of HTTP-capture files that disagree on whether "
                    "a config gate field was actually reachable -- the general form of the "
                    "28-mode2+1-mode3 inert-pool incident.")
    parser.add_argument("field", help="gate field name, e.g. control_mode (see gate_fields.py)")
    parser.add_argument("paths", nargs="+", help="capture .jsonl files to check as one pool")
    parser.add_argument("--zone", dest="zones", type=int, action="append", default=None,
                         help="check only this zone index (repeatable). Default: every zone "
                              "any file reports.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    try:
        problems = check_pool_gate_consistency(args.paths, args.field, zones=args.zones)
        inert = uniformly_unreachable_zones(args.paths, args.field, zones=args.zones)
    except PoolProvenanceError as exc:
        print(f"error: {exc}")
        return 2

    if args.json:
        print(json.dumps({
            "field": args.field, "paths": list(args.paths),
            "consistent": not problems, "problems": problems,
            "uniformly_unreachable_zones": inert,
        }, indent=2))
    else:
        if problems:
            print(f"INCONSISTENT POOL for {args.field!r}:")
            for p in problems:
                print(f"  {p}")
        else:
            print(f"consistent: every checked zone agrees on {args.field!r} reachability "
                  f"across all {len(args.paths)} file(s).")
        if inert:
            print(f"note: zone(s) {inert} are UNREACHABLE in every pooled file (uniformly "
                  "inert for this gate) -- consistent, but likely not what a campaign "
                  "studying this feature intended.")

    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
