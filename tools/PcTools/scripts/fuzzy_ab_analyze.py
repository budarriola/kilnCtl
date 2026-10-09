#!/usr/bin/env python3
"""Single entry point for the fuzzy-PID A/B campaign's conclusion.

WHY THIS SCRIPT EXISTS: this campaign (`fuzzy_ab_20260904d`, six arms
alternating `fuzzy_ab_strength50_20260903` / `fuzzy_ab_baseline_20260903` on
profile 7) is the FOURTH attempt at this question. The first two were
structurally inert (`control_mode: 2` in both presets -- the fuzzy layer,
gated on `control_mode == 3`, never ran in either arm -- commit `8906686`).
The third was valid but captured no `bd_*` telemetry, so its own data could
not prove the layer was reachable after the fact. This one captures the full
`bd_*` duty breakdown specifically so this script's step 1 can prove
reachability from the data itself, not from a memory of a live check
(`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` sec 3.6b).

Drawing the conclusion should never again be an improvisation assembled
after the fact from whatever tools happen to be lying around -- that
improvisation is exactly how two campaigns shipped inert. This script is the
one command:

    python tools/PcTools/scripts/fuzzy_ab_analyze.py \\
        [--log-dir logs/coupling] [--prefix fuzzy_ab_20260904d] \\
        [--pairs N] [--json]

Defaults match the live `fuzzy_ab_20260904d` campaign's naming
(`<prefix>_s50_run<N>.jsonl` = B / strength50, `<prefix>_base_run<N>.jsonl` =
A / baseline, `--pairs 3` for the planned six arms / three pairs).

STAGES, IN ORDER (each gates the next):

  1. bd_reachability_check, per available pair -- FIRST, because if the
     fuzzy term is bit-identical between arms, nothing downstream matters:
     the run is void regardless of what the tracking-error numbers say (this
     is precisely how the first two attempts went unnoticed). Checked field:
     bd_kp_effective/bd_ki_effective/bd_kd_effective (the fuzzy layer's
     output -- see PID_EXPANSION_PLAN.md sec 3.6/3.6b).
  2. Per-zone tracking-error comparison (pid_ab_compare) for every complete
     pair, INCLUDING that comparator's own start-temperature confound report
     (the ease-off campaign's documented finding: residual heat biases
     results -- PID_EXPANSION_PLAN.md's honesty-requirement section in
     pid_ab_compare.py).
  3. The project's >=3-zone same-direction decision rule, applied across
     whatever complete pairs exist -- the same rule the ease-off report
     applied, not a new one invented for this campaign.
  4. A final verdict line -- INDISTINGUISHABLE / DISTINGUISHABLE-ACTIONABLE
     / VOID (inert) / INCOMPLETE (missing arms) -- never silently upgraded
     past what the data actually supports.

INCOMPLETE CAMPAIGNS. This script is meant to be run against a campaign
that may not have finished -- including right now, mid-run. Missing arm
files, or a pair with only one side present, are reported as "pair N: not
yet available (missing <path>)" and skipped, not treated as an error; the
script still reports whatever complete pairs exist and states plainly how
many of the planned pairs were actually analyzed. It never fabricates a
verdict from partial data: with zero complete pairs it reports
"INCOMPLETE -- no complete pairs yet" and exits 3 rather than printing an
empty or misleading conclusion.

Exit codes: 0 = ran to a verdict (INDISTINGUISHABLE or DISTINGUISHABLE) on
at least one complete, reachable pair; 1 = at least one complete pair's
fuzzy term was INERT (bit-identical) -- the run is void, matching
bd_reachability_check's own exit code convention; 3 = no complete pairs
available yet (INCOMPLETE).
"""
from __future__ import annotations

import argparse
import json
import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from kilnctrl import bd_reachability_check as bdrc  # noqa: E402
from kilnctrl import pid_ab_compare as ab  # noqa: E402
from kilnctrl import noise_floor as nf  # noqa: E402

#: The fuzzy layer's own output fields -- narrower than bd_reachability_check's
#: twelve-field default, matching sec 3.6b's own example invocation for this
#: exact campaign (bd_kp_effective/bd_ki_effective/bd_kd_effective).
FUZZY_BD_FIELDS = ("bd_kp_effective", "bd_ki_effective", "bd_kd_effective")

DEFAULT_LOG_DIR = os.path.join(_REPO_ROOT, "logs", "coupling")
DEFAULT_PREFIX = "fuzzy_ab_20260904d"
DEFAULT_PAIRS = 3


def arm_paths(log_dir: str, prefix: str, pair_index: int) -> tuple[str, str]:
    """(A path, B path) for 1-indexed ``pair_index`` -- A=baseline,
    B=strength50, matching the campaign's own state.json labelling
    (A<n>/B<n>) and preset names."""
    b_path = os.path.join(log_dir, f"{prefix}_s50_run{pair_index}.jsonl")
    a_path = os.path.join(log_dir, f"{prefix}_base_run{pair_index}.jsonl")
    return a_path, b_path


def analyze(log_dir: str, prefix: str, n_pairs: int) -> dict:
    """Runs all stages against every pair that is fully present on disk.
    Returns a JSON-able report dict; never raises for missing files -- that
    is the expected "campaign still running" state, not an error."""
    report: dict = {
        "log_dir": log_dir, "prefix": prefix, "planned_pairs": n_pairs,
        "pairs": [],
    }

    complete_pairs = []
    for i in range(1, n_pairs + 1):
        a_path, b_path = arm_paths(log_dir, prefix, i)
        missing = [p for p in (a_path, b_path) if not os.path.isfile(p)]
        if missing:
            report["pairs"].append({
                "pair": i, "status": "not yet available",
                "missing": missing,
            })
            continue
        complete_pairs.append((i, a_path, b_path))

    if not complete_pairs:
        report["verdict"] = "INCOMPLETE -- no complete pairs yet"
        return report

    # Bound early: the two early-return paths below call the verdict-note
    # helpers before stage 3 has run, and a NameError there would crash the
    # one command this campaign's conclusion depends on.
    decision_rule_met = False
    n_analyzed = 0
    any_inert = False
    any_partially_inert = False
    any_truncated = False
    #: pair index -> zones whose fuzzy term was bit-identical (provably
    #: inert) in that pair. Those (pair, zone) votes are dropped from the
    #: >=3-zone decision rule below.
    pair_inert_zones: dict[int, list] = {}
    per_pair_zone_directions: dict[str, list[tuple[int, str]]] = {}
    # zone -> list of (pair_index, "A"/"B"/None) for whole-run iae_normalized_whole_c

    for i, a_path, b_path in complete_pairs:
        pair_report: dict = {"pair": i, "path_a": a_path, "path_b": b_path}

        # Stage 1: reachability, FIRST and gating.
        try:
            verdicts = bdrc.compare_bd_reachability(a_path, b_path, fields=FUZZY_BD_FIELDS)
            # PER-ZONE reachability, not a single pair-wide boolean. The
            # decision rule below counts ZONES, so a zone whose fuzzy term
            # was bit-identical between arms must not be allowed to cast a
            # vote -- the treatment provably never acted there. See
            # bd_reachability_check.inert_zones' own note.
            reach_zones = bdrc.reachable_zones(verdicts)
            dead_zones = bdrc.inert_zones(verdicts)
            pair_report["reachability"] = {
                "reachable": bool(reach_zones),
                "reachable_zones": reach_zones,
                "inert_zones": dead_zones,
                "report": bdrc.format_report(a_path, b_path, verdicts),
            }
            if not reach_zones:
                any_inert = True
                pair_report["status"] = "VOID (fuzzy term INERT -- bit-identical bd_* between arms)"
                report["pairs"].append(pair_report)
                continue
            if dead_zones:
                any_partially_inert = True
                pair_inert_zones[i] = dead_zones
        except bdrc.ReachabilityCheckError as exc:
            pair_report["reachability"] = {"error": str(exc)}
            pair_report["status"] = "CANNOT CHECK REACHABILITY -- refusing to proceed to a verdict for this pair"
            report["pairs"].append(pair_report)
            continue

        # Stage 2: tracking-error comparison (confound check included).
        artifact, load_reason = nf.load_artifact_diagnostic(nf.DEFAULT_ARTIFACT_PATH)
        if artifact is None and load_reason is not None:
            pair_report["noise_floor_warning"] = (
                f"noise-floor artifact NOT LOADED ({load_reason}); every verdict below is "
                "PROVISIONAL, not a confirmed result.")
        try:
            cmp_report = ab.compare_runs(a_path, b_path, noise_floor_artifact=artifact)
        except Exception as exc:  # noqa: BLE001 -- surface any comparator failure per-pair, not fatally
            pair_report["compare_error"] = str(exc)
            pair_report["status"] = "COMPARE FAILED"
            report["pairs"].append(pair_report)
            continue
        if "error" in cmp_report:
            pair_report["compare_error"] = cmp_report["error"]
            if "incomplete_arms" in cmp_report:
                any_truncated = True
                pair_report["incomplete_arms"] = cmp_report["incomplete_arms"]
                pair_report["status"] = (
                    "VOID (short/truncated arm -- EXCLUDED from the campaign verdict, "
                    "see compare_error)")
            else:
                pair_report["status"] = "COMPARE FAILED"
            report["pairs"].append(pair_report)
            continue

        pair_report["start_temp_deltas_c"] = cmp_report.get("start_temp_deltas_c")
        pair_report["compare_text"] = ab.format_compare_text(cmp_report)
        pair_report["status"] = "analyzed"
        report["pairs"].append(pair_report)

        for comparison in cmp_report.get("comparisons", []):
            if comparison.segment is not None or comparison.metric != "iae_normalized_whole_c":
                continue
            zone = comparison.zone
            if zone in pair_inert_zones.get(i, ()):  # provably inert here -- no vote
                continue
            direction = comparison.better if comparison.distinguishable else None
            per_pair_zone_directions.setdefault(str(zone), []).append((i, direction))

    report["any_pair_inert"] = any_inert
    report["any_pair_partially_inert"] = any_partially_inert
    report["pair_inert_zones"] = {str(k): v for k, v in pair_inert_zones.items()}
    report["any_pair_truncated"] = any_truncated

    def _with_inert_zone_note(verdict: str) -> str:
        if not any_partially_inert:
            return verdict
        return verdict + (
            " (NOTE: PARTIALLY INERT -- the fuzzy term was bit-identical between arms in "
            f"{report['pair_inert_zones']} (pair -> zones); those zones were EXCLUDED from the "
            ">=3-zone decision rule, since the treatment provably never acted in them. The "
            "rule was therefore evaluated over FEWER zones than the campaign planned -- "
            "treat any DISTINGUISHABLE call here as provisional and re-check the wiring.)")

    # REPLICATION / MULTIPLICITY CAVEAT. The >=3-zone rule counts ZONES, and
    # three zones of one kiln in ONE pair are not three independent
    # experiments -- pid_ab_compare.summarize_multiplicity puts the per-key
    # false-positive rate of the range floor at ~30.5% at n=3 and ~12% at
    # n=6, with no correction applied. A rule satisfied by a single pair is
    # therefore an unreplicated finding, and the verdict must say so rather
    # than reading as a campaign-wide result. (Each pair's own multiplicity
    # block is printed above with its compare_text; this is the campaign-
    # level statement of the same problem.)

    def _with_replication_note(verdict: str) -> str:
        if not decision_rule_met:
            return verdict
        if n_analyzed >= 2:
            return verdict
        return verdict + (
            f" (CAVEAT: the decision rule is satisfied by {n_analyzed} analyzed pair -- "
            "UNREPLICATED. The three zones of one firing are not three independent "
            "experiments, and no multiplicity correction is applied to the per-key verdicts "
            "(the range floor's own per-key false-positive rate is ~30.5% at n=3, ~12% at "
            "n=6 -- see each pair's MULTIPLICITY block above). Do not act on this until a "
            "second pair reproduces it in the same direction.)")

    def _with_truncated_note(verdict: str) -> str:
        verdict = _with_replication_note(_with_inert_zone_note(verdict))
        if not any_truncated:
            return verdict
        return verdict + (
            " (NOTE: at least one pair had a short/truncated arm and was EXCLUDED from this "
            "verdict -- see the per-pair 'VOID (short/truncated arm ...)' status above; that "
            "pair's data was never averaged in.)")

    complete_analyzed = [p for p in report["pairs"] if p.get("status") == "analyzed"]
    n_analyzed = len(complete_analyzed)
    if not complete_analyzed and any_inert:
        report["verdict"] = _with_truncated_note(
            "VOID -- at least one complete pair's fuzzy term was bit-identical between arms "
            "(INERT). Do not trust any tracking-error conclusion from this campaign until the "
            "wiring is fixed and re-fired. See per-pair reachability reports above.")
        return report
    if not complete_analyzed:
        report["verdict"] = _with_truncated_note("INCOMPLETE -- no complete, reachable pairs yet")
        return report

    # Stage 3: >=3-zone same-direction decision rule, across whatever
    # complete pairs exist (same rule as the ease-off report).
    consistent_zones_b = [z for z, dirs in per_pair_zone_directions.items()
                           if dirs and all(d[1] == "B" for d in dirs)]
    consistent_zones_a = [z for z, dirs in per_pair_zone_directions.items()
                           if dirs and all(d[1] == "A" for d in dirs)]
    report["whole_run_iae_directions_by_zone"] = per_pair_zone_directions

    n_zones_seen = len({z for z in per_pair_zone_directions})
    report["zones_voting"] = n_zones_seen
    decision_rule_met = (len(consistent_zones_b) >= 3) or (len(consistent_zones_a) >= 3)


    if any_inert:
        report["verdict"] = (
            "MIXED -- at least one pair analyzed cleanly, but at least one OTHER pair's fuzzy "
            "term was INERT. Do not draw a campaign-wide conclusion until every pair is "
            "reachable; treat this as VOID until re-fired.")
    elif len(complete_analyzed) < n_pairs:
        report["verdict"] = (
            f"INCOMPLETE ({len(complete_analyzed)}/{n_pairs} pairs analyzed) -- provisional "
            "reading only, campaign still running:")
        if decision_rule_met:
            report["verdict"] += " decision rule currently MET on partial data (re-check on completion)."
        else:
            report["verdict"] += " decision rule not yet met on partial data."
    elif decision_rule_met:
        winner = "B (strength50)" if consistent_zones_b else "A (baseline)"
        report["verdict"] = (
            f"DISTINGUISHABLE -- {winner} wins iae_normalized_whole_c in >=3 zones, same "
            "direction, across all analyzed pairs. Check reported magnitudes against the "
            "0.5C actionable bar before acting.")
    else:
        report["verdict"] = (
            "INDISTINGUISHABLE -- no metric shows a same-direction, >=3-zone-consistent "
            "difference across all analyzed pairs. Per project decision rule, report only, "
            "do not act.")

    report["verdict"] = _with_truncated_note(report["verdict"])

    return report


def format_text(report: dict) -> str:
    lines = [f"fuzzy A/B campaign analysis: {report['prefix']} ({report['log_dir']})", ""]
    for p in report["pairs"]:
        lines.append(f"=== pair {p['pair']}: {p['status']} ===")
        if p.get("reachability", {}).get("inert_zones"):
            lines.append(
                f"  PARTIALLY INERT: zones {p['reachability']['inert_zones']} bit-identical "
                f"between arms -- EXCLUDED from the decision rule (reachable zones: "
                f"{p['reachability'].get('reachable_zones')})")
        if "missing" in p:
            for m in p["missing"]:
                lines.append(f"  missing: {m}")
        if "reachability" in p and "report" in p["reachability"]:
            lines.append(p["reachability"]["report"])
        if "reachability" in p and "error" in p["reachability"]:
            lines.append(f"  reachability error: {p['reachability']['error']}")
        if "start_temp_deltas_c" in p:
            lines.append(f"  start-temp deltas (C): {p['start_temp_deltas_c']}")
        if "compare_text" in p:
            lines.append(p["compare_text"])
        if "compare_error" in p:
            lines.append(f"  compare error: {p['compare_error']}")
        lines.append("")
    lines.append(f"VERDICT: {report.get('verdict', '(none)')}")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Single entry point for the fuzzy-PID A/B campaign's conclusion: "
                    "bd_reachability_check FIRST (gating), then per-zone tracking-error "
                    "comparison, then the >=3-zone decision rule. Handles an incomplete "
                    "campaign gracefully -- see module docstring.")
    parser.add_argument("--log-dir", default=DEFAULT_LOG_DIR)
    parser.add_argument("--prefix", default=DEFAULT_PREFIX)
    parser.add_argument("--pairs", type=int, default=DEFAULT_PAIRS)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    report = analyze(args.log_dir, args.prefix, args.pairs)

    if args.json:
        print(json.dumps(report, indent=2, default=str))
    else:
        print(format_text(report))

    verdict = report.get("verdict", "")
    if verdict.startswith("VOID") or verdict.startswith("MIXED"):
        return 1
    if verdict.startswith("INCOMPLETE"):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
