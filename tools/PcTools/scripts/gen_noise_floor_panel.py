#!/usr/bin/env python3
"""Regenerate the `noise_floor` block of
tools/PcTools/config_presets/tuning_recommendations.json from the
checked-in tools/PcTools/config_presets/noise_floor.json artifact.

WHY THIS SCRIPT EXISTS: commit 72cac87 hand-wrote the noise_floor block from
figures computed against a contaminated noise_floor.json (daea3bc fixed the
contamination -- log_analysis._zone_samples_from_exec_body was reading
actual_c while ignoring actual_valid, so the firmware's 0.0 "no reading yet"
placeholder leaked into ramp_worst_error_c's segment-0 floor as a spurious
~27C outlier). Hand-copying numbers a second time would just go stale a
second time. Instead this script re-derives every number and every
reliable/unreliable classification straight from noise_floor.json, using the
SAME rule kilnctrl.pid_ab_compare already applies when it gates a
DISTINGUISHABLE/INDISTINGUISHABLE verdict on a metric's floor stability:
summarize_metric_floor_reliability() and its FLOOR_RELIABILITY_RATIO
threshold. This script does not reimplement that rule -- it imports it, so
the panel and the A/B comparator can never silently disagree on what counts
as "reliable".

Run:
    python tools/PcTools/scripts/gen_noise_floor_panel.py
        [--noise-floor PATH] [--out PATH] [--check]

--check exits 1 (printing a diff-relevant message) if regenerating the block
from the current noise_floor.json would change tuning_recommendations.json
-- use this in a pre-flight check to catch exactly the kind of staleness
this script was written to fix.
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

from kilnctrl.pid_ab_compare import (  # noqa: E402
    FLOOR_RELIABILITY_RATIO,
    _range_floor_false_positive_rate,
    summarize_metric_floor_reliability,
)

_DEFAULT_NOISE_FLOOR = os.path.join(
    _REPO_ROOT, "tools", "PcTools", "config_presets", "noise_floor.json")
_DEFAULT_OUT = os.path.join(
    _REPO_ROOT, "tools", "PcTools", "config_presets", "tuning_recommendations.json")

#: The most common per-metric sample size in this artifact (6 of the 8
#: metrics have n=6; iae_normalized_whole_c has n=3 because it is a
#: whole-run figure with only one entry per zone). Used to report a single
#: representative per-key false-positive rate for the multiplicity caveat --
#: matches the ~48-key family this report's numbers describe (n=6 covers
#: the large majority of keys).
_REPRESENTATIVE_N = 6

#: Unit for each metric family -- not present in noise_floor.json (whose
#: noise_floor_c field name is a misnomer for non-temperature metrics), so
#: it is supplied here rather than guessed at render time.
_UNITS = {
    "iae_normalized_whole_c": "C",
    "iae_normalized_c": "C",
    "dwell_entry_time_to_peak_s": "s",
    "dwell_entry_overshoot_peak_c": "C",
    "dwell_steady_state_offset_c": "C",
    "ramp_mean_error_c": "C",
    "settle_time_s": "s",
    "ramp_worst_error_c": "C",
}


def _round3(x: float) -> float:
    return round(x, 3)


def build_noise_floor_block(noise_floor_artifact: dict) -> dict:
    summary = summarize_metric_floor_reliability(noise_floor_artifact)
    start = noise_floor_artifact.get("start_conditions", {})
    per_key_alpha = _range_floor_false_positive_rate(_REPRESENTATIVE_N)

    metrics = {}
    # Order metrics by ratio ascending so the tightest (most reliable)
    # cluster reads first -- purely cosmetic, does not affect classification.
    for metric, stats in sorted(summary.items(), key=lambda kv: (kv[1]["ratio"] is None, kv[1]["ratio"] or 0)):
        if stats["ratio"] is None:
            continue  # fewer than 2 entries -- not enough data to judge, omit rather than guess
        ratio = stats["ratio"]
        reliable = stats["reliable"]
        note = "{:.2f}x spread across zones/segments".format(ratio)
        note += " -- reliable (<= {:.1f}x)".format(FLOOR_RELIABILITY_RATIO) if reliable \
            else " -- UNSTABLE (> {:.1f}x): floor too unstable to support a conclusion".format(FLOOR_RELIABILITY_RATIO)
        metrics[metric] = {
            "reliable": reliable,
            "min_c": _round3(stats["floor_min"]),
            "max_c": _round3(stats["floor_max"]),
            "ratio": _round3(ratio),
            "unit": _UNITS.get(metric, "C"),
            "note": note,
        }

    block = {
        "rig": "bench fixture (~60C peak firing) -- not a claim about a full-size kiln at cone temperature",
        "source": "tools/PcTools/config_presets/noise_floor.json",
        "generated_from": list(noise_floor_artifact.get("generated_from", [])),
        "n_repeats": noise_floor_artifact.get("n_repeats"),
        "like_for_like": start.get("like_for_like"),
        "like_for_like_threshold_c": start.get("like_for_like_threshold_c"),
        "start_temp_c_range": _round3(start.get("start_temp_c_range")) if start.get("start_temp_c_range") is not None else None,
        "warning": start.get("warning"),
        "stat_caveat": ("Each floor is the max-minus-min RANGE across the repeat captures. That statistic "
                         "grows with sample size, so it is not a fixed property of the kiln and is not "
                         "directly comparable to a floor built from a different number of runs."),
        "transfer_caveat": ("Measured on the reference bench rig only, at bench temperatures. It does not "
                             "transfer to your own kiln, geometry, or firing temperature -- your own repeat "
                             "campaign (pid_ab_compare.py / noise_floor.py) is what would give you your own floor."),
        "reliability_rule": (
            "A metric is 'reliable' when the ratio of its largest to smallest measured floor across all "
            "(zone, segment) entries is <= FLOOR_RELIABILITY_RATIO ({:.1f}); see "
            "kilnctrl.pid_ab_compare.summarize_metric_floor_reliability, the same rule pid_ab_compare.py "
            "itself uses to flag an UNSTABLE floor before trusting a DISTINGUISHABLE/INDISTINGUISHABLE "
            "verdict."
        ).format(FLOOR_RELIABILITY_RATIO),
        "multiplicity_caveat": (
            "This report scores dozens of (zone, metric, segment) keys at once with no multiplicity "
            "correction. At the sample size most keys here have (n={n}), a single key crossing its own "
            "floor happens by chance alone about {pct:.0f}% of the time even when nothing real changed "
            "(kilnctrl.pid_ab_compare._range_floor_false_positive_rate({n})) -- across a ~48-key report "
            "that adds up fast. A lone flagged metric is not evidence; the same metric moving the same "
            "direction across several zones is the one pattern worth trusting."
        ).format(n=_REPRESENTATIVE_N, pct=per_key_alpha * 100.0),
        "metrics": metrics,
    }
    return block


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--noise-floor", default=_DEFAULT_NOISE_FLOOR)
    ap.add_argument("--out", default=_DEFAULT_OUT)
    ap.add_argument("--check", action="store_true",
                     help="exit 1 if regenerating would change --out, without writing")
    args = ap.parse_args()

    with open(args.noise_floor, "r", encoding="utf-8") as f:
        nf_artifact = json.load(f)

    with open(args.out, "r", encoding="utf-8") as f:
        rec = json.load(f)

    new_block = build_noise_floor_block(nf_artifact)

    if args.check:
        if rec.get("noise_floor") != new_block:
            print("STALE: tuning_recommendations.json's noise_floor block does not match "
                  "what {} currently derives -- rerun without --check to regenerate.".format(args.noise_floor))
            return 1
        print("OK: noise_floor block is current with " + args.noise_floor)
        return 0

    rec["noise_floor"] = new_block
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")
    print("Wrote noise_floor block to {} from {}".format(args.out, args.noise_floor))
    for metric, m in new_block["metrics"].items():
        print("  {:32s} reliable={!s:5} ratio={:.2f}x floor=[{}, {}] {}".format(
            metric, m["reliable"], m["ratio"], m["min_c"], m["max_c"], m["unit"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
