"""A/B comparison of two firing captures for the fuzzy-layer question in
firmware/KilnFW/docs/PID_EXPANSION_PLAN.md §3.6/§3.7: does turning the fuzzy
layer on (``strength_pct=50``) measurably help profile-7 tracking versus the
same build with it off (``strength_pct=0``)?

Reuses ``log_analysis.py``'s windowing/IAE machinery unmodified -- this
module only (a) accepts an HTTP-capture path via ``http_capture_log.py``'s
parser and (b) assembles the per-zone metric set PID_EXPANSION_PLAN.md's
§2 table already reports, so numbers here are comparable to that table:

  * whole-run and per-segment normalized IAE (``log_analysis``'s own
    definition -- time-weighted mean |error|, °C -- the same one every §2/§3
    number in the plan was generated with; see log_analysis.py's module
    docstring for why that is deliberately simpler than the firmware's own
    ``iae_normalized`` and used for windowed stats throughout this package).
  * ramp tracking error, mean and worst, per segment (worst = the larger of
    that segment's own max overshoot/undershoot magnitude).
  * dwell-entry overshoot, peak and time-to-peak after the ramp/dwell
    boundary, per segment (``log_analysis.ramp_to_dwell_transitions``).
  * dwell steady-state offset per segment (the dwell window's own
    time-weighted mean error -- by the time a dwell window is being
    computed the ramp-entry transient has already been captured separately
    by the overshoot-peak metric above, so the window mean is a reasonable
    "settled" figure without needing a second, narrower sub-window).
  * settle time per segment (``log_analysis.ramp_to_dwell_transitions``
    again).

THE HONESTY REQUIREMENT (§3.3's "Iterative tuning" item, §3.7). This kiln's
run-to-run noise floor has never been measured. The one candidate pair in
the fixtures that could have supplied it (``holdfix_clean`` vs ``final``)
is itself confounded by a 4.8 °C start-temperature difference and produced
+22.5/+47.5/+28.6 % swings -- so it is evidence that a several-degree start
delta CAN produce a large swing, not a measurement of the noise floor
itself (the swing it produced is not attributable to noise alone; the
confound is uncontrolled for in that pair too). There is no reliable way to
turn that one confounded data point into a per-degree sensitivity number
without pretending to know something we don't.

So this module does the only honest thing available: it reports the
starting-temperature delta between the two runs, per zone, and REFUSES to
declare a winner for any zone whose start-temp delta exceeds
``CONFOUND_THRESHOLD_C`` (default 1.0 °C -- deliberately tight, since the
only data point we have shows a 4.8 °C delta is already large enough to
dominate). Below that threshold it still never claims a confirmed win --
every verdict is printed as PROVISIONAL with an explicit "noise floor:
UNKNOWN" caveat, because a small start-temp delta rules out ONE known
confounder, not every possible source of run-to-run variance.

2026-09-02f -- THE START-TEMPERATURE CONFOUND IS NOT HYPOTHETICAL. Measured
directly: the "rested" precondition is relative to the thermocouple cold
junction, which lives on the board and self-heats -- it certifies "cooled to
wherever the board is now", not "cooled to ambient". Across the checked-in
6-run repeat campaign (was 3 runs as of this note's original writing),
start temperatures span 27.60-28.88 °C -- a 1.29 °C range against the
1.00 °C ``CONFOUND_THRESHOLD_C`` this module already refuses on; across the
wider set of captures on hand the spread reaches 3.9 °C. The owner's decision:
keep firing the same protocol (no change to the rested precondition) and
instead make the comparison machinery ACCOUNT for the confound rather than
pretend it isn't there. Two additions, both deliberately conservative:

  * :func:`fit_start_temp_sensitivity` -- an OLS regression of
    ``iae_normalized_whole_c`` (the one metric this rig's own noise-floor
    measurement shows has a comparatively tight, trustworthy floor -- see
    the module-level note below) on start temperature, fit ONLY across a set
    of captures the caller asserts are genuinely the same controller
    configuration (typically the noise-floor artifact's own
    ``generated_from`` repeat set, where by construction nothing but start
    temperature and run-to-run noise should differ). With as few as 3-6
    points this is not a precise instrument -- it is reported as a rough
    magnitude, with its own ``n`` and Pearson ``r`` printed everywhere it is
    used, and it refuses to fit at all below ``MIN_N_FOR_SENSITIVITY`` (3)
    points. It is used ONLY to show the reader how much of a raw A/B delta
    the known start-temp difference could explain (raw delta, predicted
    contribution from the fitted sensitivity, and the residual) -- it is
    reported ALONGSIDE the existing REFUSED/INDISTINGUISHABLE/PROVISIONAL
    verdict and never overrides it. An adjustment that could flip a REFUSED
    or INDISTINGUISHABLE comparison into a confident one is exactly the
    failure mode this module exists to prevent, so the verdict is computed
    from the RAW delta and the measured floor only, same as before; the
    fitted sensitivity is supplementary context, not a gate.
  * :func:`summarize_metric_floor_reliability` -- some metrics' measured
    floors are internally consistent across zones/segments (tight), others
    are not (the same metric's floor varies several-fold from one zone or
    segment to the next, meaning the "floor" itself is noisy and a
    DISTINGUISHABLE/INDISTINGUISHABLE call built on it should be read with
    that in mind). Computed directly from the noise-floor artifact's own
    entries.

2026-09-03 -- ADVERSARIAL STATISTICAL REVIEW, FIVE FINDINGS ACTED ON.

  1. NO MULTIPLICITY CONTROL. A comparison evaluates 48 (zone, metric,
     segment) keys, each with its own ~12%-at-n=6 per-key false-positive
     rate (see below) and no correction. :func:`summarize_multiplicity`
     reports the family size, the per-key false-positive rate for the n
     actually backing each key, and the probability at least one
     DISTINGUISHABLE verdict appears by chance alone -- both under an
     (overstated) independence assumption and at the reviewer's estimated
     effective family size (10-15, keys are correlated). It flags the one
     pattern this module treats as potentially actionable: the SAME metric
     DISTINGUISHABLE in the SAME direction across several zones/segments.
     A single DISTINGUISHABLE key is, on its own, close to the expected
     outcome under pure chance -- not a finding.
  2. THE RANGE FLOOR IS NOT SCALE-STABLE ACROSS n. E[range]/sigma grows
     with n (1.69 at n=3, 2.53 at n=6, 3.07 at n=10), so floors built from
     different n are not on the same scale -- and the checked-in artifact
     already mixes them (``z0:settle_time_s:1`` has n=3, every other key
     has n=6). Every :class:`MetricComparison` now carries ``floor_n``, and
     :func:`prediction_interval_floor` offers a scale-stable alternative --
     ``t(.975, n-1) * std_c * sqrt(2)``, the honest answer to "could two new
     same-config runs differ by this much?" -- reported alongside the range
     floor as ``pi_floor_c`` / ``pi_distinguishable`` on every comparison.
     THIS IS INFORMATIONAL ONLY: the PI floor is WIDER than the range floor
     (t*sqrt(2) versus a bare range), so switching to it as the verdict
     floor would lose some currently-DISTINGUISHABLE findings. That is the
     statistically correct direction, but it is the owner's call, not a
     silent default -- ``distinguishable``/the printed verdict still use
     the range floor exactly as before; ``pi_distinguishable`` is reported
     next to it so the cost of switching is visible on every key.
  3. START-TEMPERATURE UNIT MISMATCH. ``noise_floor.extract_start_conditions``
     computes ``start_temp_c_mean`` as the mean of ALL status channels in the
     raw HTTP body (used for the artifact's own ``start_conditions`` /
     ``LIKE_FOR_LIKE_THRESHOLD_C`` check). ``_first_valid_start_temp`` below
     computes the PER-ZONE first valid ``actual_c`` (used for
     ``CONFOUND_THRESHOLD_C`` / the REFUSED gate). Both constants happen to
     be 1.0 °C, but they gate two DIFFERENT quantities -- an all-channel
     mean versus one zone's own reading -- computed by two different modules
     for two different purposes. This module cannot change
     ``noise_floor.py`` (out of scope for this pass), so the fix here is to
     stop letting the shared "1.0 °C" value read as "the same measurement":
     see ``START_TEMP_METRIC_NOTE`` below, printed by every text report.
  4. THE 1.0 °C CONFOUND-THRESHOLD JUSTIFICATION WAS AN ANECDOTE (a single
     4.8 °C confounded pair bounds nothing). The real justification: fitting
     ``fit_start_temp_sensitivity`` against the checked-in 6-run repeat set
     gives per-zone sensitivities of roughly 0.009-0.133 °C per °C of start
     delta; at the 1.0 °C threshold that predicts a 0.009-0.133 °C shift in
     ``iae_normalized_whole_c``, against that same metric's measured
     whole-run floors of 0.077-0.147 °C. The top of the predicted range
     (0.133 °C) falls inside the measured-floor range (0.077-0.147 °C) --
     i.e. 1.0 °C is roughly where the confound's OWN predicted contribution
     reaches the noise floor, not an arbitrary round number.
     ``CONFOUND_THRESHOLD_C`` is UNCHANGED; only its justification is fixed.
  5. STALE FIGURES. Every number quoted in this module and in
     ``PID_EXPANSION_PLAN.md`` SS3.3 that predates the 6-run repeat
     campaign (``logs/coupling/noise_floor_p7*_run*.jsonl``) has been
     re-measured against it -- see ``FLOOR_RELIABILITY_RATIO`` below for the
     retuned reliability threshold, and the plan doc for the refreshed
     ratio table.

2026-09-03b -- ``CONFOUND_THRESHOLD_C`` RAISED TO 1.5C AND TIED TO
run_queue.py. A live paired A/B run held arm B against run_queue.py's
pairing gate for 75 minutes and still could not match; the root cause
turned out to be that this rig's passive cooling asymptotes 1.4-1.5C above
the previous session's start and FLATLINES there (measured directly --
see run_queue.DEFAULT_PAIR_START_TOL_C's docstring), so a tolerance below
that floor is not achievable at all, not just strict. Fixing run_queue.py's
tolerance without also moving this module's own (then-1.0C) gate would have
reopened the ORIGINAL PROBLEM 3 mismatch in the other direction -- a pair
the queue now accepts as paired could still get REFUSED here for exceeding
a stricter, independently-chosen number. ``CONFOUND_THRESHOLD_C`` is now
``= run_queue.DEFAULT_PAIR_START_TOL_C`` (imported, not duplicated) so the
two gates cannot drift apart again. See that constant's own docstring for
the full derivation, including the check against the owner's stated
"sub-0.5C noise is not actionable" bar.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import random
from collections import Counter
from typing import Optional, Sequence

from kilnctrl import log_analysis as la
from kilnctrl import http_capture_log as hc
from kilnctrl import noise_floor as nf
from kilnctrl import run_queue as rq
from kilnctrl import profile_stabilization as ps

#: 2026-09-03 REVISED to 1.5C (was 1.0C), and DERIVED FROM
#: run_queue.DEFAULT_PAIR_START_TOL_C rather than chosen independently.
#:
#: PROBLEM: this module's own 1.0C gate and run_queue.py's (then-)0.8C
#: pairing tolerance told two different stories about the same rig -- a
#: pair the run queue would accept as "paired" could still land above this
#: module's refusal boundary, and vice versa. Found while re-deriving
#: run_queue's tolerance from measured cooldown data: this rig's passive
#: cooling asymptotes 1.4-1.5C above the previous session's start and then
#: FLATLINES (see run_queue.DEFAULT_PAIR_START_TOL_C's docstring for the
#: measured numbers) -- a hard physical floor, not a noise artifact. A
#: 1.0C confound gate sitting BELOW that floor would refuse essentially
#: every real pair this rig can produce, which is exactly the failure this
#: whole revision exists to fix: a paired arm held for 75 minutes against a
#: too-tight gate and still could not match.
#:
#: So the two thresholds are now the SAME constant, imported rather than
#: independently chosen, so they cannot drift apart again: a pair
#: run_queue.py accepts as paired is, by construction, never refused here
#: for exceeding a DIFFERENT number.
#:
#: Justified against the owner's actual bar ("i dont care about sub 0.5C
#: noise"), not just against feasibility: the fitted start-temp
#: sensitivities (roughly 0.009-0.133 C/C on the checked-in 6-run repeat
#: set -- see fit_start_temp_sensitivity) times 1.5C predict a confound
#: contribution of at most 1.5 * 0.133 = 0.20C in the most sensitive zone --
#: comfortably under the 0.5C the owner has said is not actionable, and on
#: the same order as this rig's own measured whole-run IAE noise floors
#: (z0 0.116 / z1 0.077 / z2 0.147C). 1.5C is therefore the loosest
#: threshold this rig can actually produce pairs under AND still admits
#: only a sub-actionable, near-noise-floor confound -- not "give up and let
#: anything through".
CONFOUND_THRESHOLD_C = rq.DEFAULT_PAIR_START_TOL_C

#: PROBLEM 3 (module docstring): this module's confound gate compares each
#: zone's PER-ZONE first-valid ``actual_c`` against CONFOUND_THRESHOLD_C.
#: noise_floor.py's own LIKE_FOR_LIKE_THRESHOLD_C (1.0 C, out of scope to
#: change here -- and no longer the same number as CONFOUND_THRESHOLD_C
#: since the 2026-09-03 revision to 1.5C) instead gates the MEAN across ALL
#: status channels (noise_floor.extract_start_conditions). Both are about
#: start-temperature drift, but they are neither the same measurement NOR
#: (any more) the same threshold -- printed on every text report so a
#: reader does not conflate them.
START_TEMP_METRIC_NOTE = (
    "START-TEMP UNIT NOTE: the per-zone deltas below (and the "
    f"{CONFOUND_THRESHOLD_C:.1f}C confound gate) use each zone's own "
    "first-valid actual_c. noise_floor.py's separate 'like-for-like' check "
    "(a DIFFERENT threshold, LIKE_FOR_LIKE_THRESHOLD_C=1.0C) uses a "
    "DIFFERENT quantity too -- the mean across ALL status channels from the "
    "raw capture. Neither the threshold nor the quantity are interchangeable "
    "between the two checks; do not read a pass/fail on one as a pass/fail "
    "on the other."
)

NOISE_FLOOR_NOTE = (
    "NOISE FLOOR: UNKNOWN. Never measured on this rig (PID_EXPANSION_PLAN.md "
    "SS3.3 'Iterative tuning', SS3.7). The only candidate data point "
    "(holdfix_clean vs final) is itself confounded by a 4.8C start-temperature "
    "delta and produced +22.5/+47.5/+28.6% swings -- evidence that start-temp "
    "alone can dominate, not a calibrated noise floor. Nothing below is a "
    "confirmed result."
)

NOISE_FLOOR_KNOWN_NOTE = (
    "NOISE FLOOR: measured (tools/PcTools/config_presets/noise_floor.json, see "
    "noise_floor.py / PID_EXPANSION_PLAN.md SS3.3). A per-(zone, metric, "
    "segment) difference smaller than its measured floor is reported "
    "INDISTINGUISHABLE below, not PROVISIONAL -- it is not attributable to "
    "whatever this A/B comparison is testing. A key with no floor entry "
    "(never covered by the repeat campaign) still falls back to PROVISIONAL."
)

#: Minimum number of (start_temp_c, iae_normalized_whole_c) points required
#: before ``fit_start_temp_sensitivity`` will fit a regression at all. 3 is
#: already an honest floor, not a good one -- a 3-point OLS fit has one
#: residual degree of freedom, so its slope is reported everywhere alongside
#: its own n and Pearson r rather than presented as a settled number.
MIN_N_FOR_SENSITIVITY = 3

#: The one metric ``fit_start_temp_sensitivity`` fits. Chosen because this
#: rig's own noise-floor measurement shows it has a comparatively tight,
#: internally-consistent floor across zones/segments (see
#: ``summarize_metric_floor_reliability``); every other metric in
#: ``noise_floor.METRIC_NAMES`` (dwell_steady_state_offset_c most of all)
#: has a floor noisy enough that fitting a "sensitivity" to it would mostly
#: be fitting noise.
SENSITIVITY_METRIC = "iae_normalized_whole_c"

#: A metric whose measured floor varies by more than this ratio (max/min,
#: across every (zone, segment) entry for that metric in the artifact) is
#: flagged as having an UNSTABLE floor -- the DISTINGUISHABLE/
#: INDISTINGUISHABLE call for that metric is only as trustworthy as the
#: floor it's gated on.
#:
#: 2026-09-03 RE-TUNED against the checked-in 6-run repeat campaign
#: (logs/coupling/noise_floor_p7*_run*.jsonl, replacing the 3-run set the
#: original 5.0 was picked against). The old value no longer separates the
#: two clusters cleanly on real n>=5 data -- ramp_mean_error_c (was
#: "reliable" at ~4.5x) is now 5.49x and dwell_entry_time_to_peak_s (was
#: "unstable" at ~8-30x) is now 4.73x, both straddling the old boundary.
#: 2026-09-03, SAME DAY -- these ratios were recomputed a second time after
#: fixing a separate defect in log_analysis's HTTP-capture parser (it read
#: actual_c while ignoring actual_valid, so the firmware's 0.0 "no reading
#: yet" placeholder leaked into ramp_worst_error_c's segment-0 floor as a
#: spurious ~27C outlier on 5 of 6 captures). The 6-run measured ratios,
#: current as of the artifact regenerated after that fix:
#:   iae_normalized_whole_c        1.91x   (tight cluster)
#:   ramp_worst_error_c            2.57x   -- WAS 117.87x/"cannot resolve
#:                                             anything" before the parser
#:                                             fix; that framing is now wrong
#:   iae_normalized_c              4.30x
#:   dwell_entry_time_to_peak_s    4.73x
#:   ramp_mean_error_c             5.49x
#:   ---------------------------- gap ----------------------------
#:   dwell_entry_overshoot_peak_c  6.55x   (unstable cluster)
#:   dwell_steady_state_offset_c  13.98x
#:   settle_time_s                15.27x
#: The only clean gap in that list is between 5.49x and 6.55x -- unchanged
#: by the parser fix, since only segment-0 windows (and so only
#: ramp_worst_error_c / ramp_mean_error_c / iae_normalized_c /
#: iae_normalized_whole_c) contained the placeholder sample. Threshold
#: stays at 6.0 (was 5.0) to fall inside that gap rather than inside either
#: cluster. Re-check again once a real campaign lands with materially more
#: than 6 repeats -- these ratios are themselves computed from n=6 (n=3 for
#: iae_normalized_whole_c, which only has 3 zone-level entries) and are not
#: settled numbers. tools/PcTools/config_presets/tuning_recommendations.json
#: and the zones_page.html panel built from the PRE-fix floors still say
#: ramp_worst_error_c "cannot resolve anything useful" -- that is now false
#: and those need regenerating (out of scope for this module).
FLOOR_RELIABILITY_RATIO = 6.0


# ---------------------------------------------------------------------------
# Per-run, per-zone metrics
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ZoneRunMetrics:
    zone: int
    start_temp_c: float
    iae_normalized_whole_c: float
    iae_normalized_by_segment: dict  # segment_index -> float
    ramp_mean_error_c: dict          # segment_index -> float
    ramp_worst_error_c: dict         # segment_index -> float
    dwell_entry_overshoot_peak_c: dict     # segment_index -> Optional[float]
    dwell_entry_time_to_peak_s: dict       # segment_index -> Optional[float]
    dwell_steady_state_offset_c: dict      # segment_index -> Optional[float]
    settle_time_s: dict                    # segment_index -> Optional[float]


def _segment_normalized_iae(rows: Sequence[la.PollRow], windows: Sequence[la.Window], zone: int) -> dict:
    """Combine each segment's ramp + dwell windows into one normalized IAE,
    matching how PID_EXPANSION_PLAN.md's per-segment rows (e.g. "dwell-entry
    overshoot, seg0") are keyed by segment, not by ramp/dwell separately."""
    agg: dict = {}
    for w in windows:
        s = la.window_zone_stats(w, rows, zone)
        if s is None:
            continue
        entry = agg.setdefault(w.segment_index, {"iae_raw": 0.0, "duration": 0.0})
        entry["iae_raw"] += s.iae_raw_c_s
        entry["duration"] += w.duration_s
    return {
        seg: (v["iae_raw"] / v["duration"] if v["duration"] > 0 else float("nan"))
        for seg, v in agg.items()
    }


def compute_zone_metrics(
    rows: Sequence[la.PollRow], zone: int, start_temp_c: float, band_c: float = 1.0,
    min_segment_index: int = 0,
) -> Optional[ZoneRunMetrics]:
    """``min_segment_index`` -- THE SCORED-WINDOW CONFOUND FIX (PID_EXPANSION_PLAN.md,
    "A/B campaign ambient-confound protocol"). Segments below this index are
    excluded from every metric here: the "whole" window, the per-segment
    dicts, and the ramp-to-dwell transitions. Rows from excluded segments
    are NOT deleted from ``rows`` -- ``la.build_windows``/``la.window_zone_stats``
    still need the full row sequence for correct elapsed-time math -- they
    are just excluded from what gets aggregated into the returned metrics.

    Default 0 (score everything, today's behaviour) so every existing caller
    is unaffected. Pass 1 when segment 0 is a stabilisation hold prepended
    ahead of the profile under test (see ``STABILIZATION_SEGMENT_INDEX``)
    to score only the segments that begin from the controlled stabilisation
    temperature, not the uncontrolled room-ambient start.
    """
    windows = la.build_windows(rows)
    if not windows:
        return None
    windows = [w for w in windows if w.segment_index >= min_segment_index]
    if not windows:
        return None

    whole = la.Window(
        -1, "all", windows[0].start_idx, windows[-1].end_idx,
        rows[windows[0].start_idx].elapsed_s, rows[windows[-1].end_idx].elapsed_s,
    )
    whole_stats = la.window_zone_stats(whole, rows, zone)
    if whole_stats is None:
        return None

    iae_by_segment = _segment_normalized_iae(rows, windows, zone)

    ramp_mean: dict = {}
    ramp_worst: dict = {}
    dwell_offset: dict = {}
    for w in windows:
        s = la.window_zone_stats(w, rows, zone)
        if s is None:
            continue
        if w.phase == "ramp":
            ramp_mean[w.segment_index] = s.mean_error_c
            ramp_worst[w.segment_index] = max(s.max_overshoot_c, s.max_undershoot_c)
        elif w.phase == "dwell":
            dwell_offset[w.segment_index] = s.mean_error_c

    transitions = [
        ev for ev in la.ramp_to_dwell_transitions(rows, windows, zone, band_c=band_c)
        if ev.segment_index >= min_segment_index
    ]
    overshoot_peak: dict = {}
    time_to_peak: dict = {}
    settle_time: dict = {}
    for ev in transitions:
        overshoot_peak[ev.segment_index] = ev.peak_overshoot_c
        time_to_peak[ev.segment_index] = (
            (ev.peak_overshoot_at_s - ev.transition_at_s) if ev.peak_overshoot_at_s is not None else None
        )
        settle_time[ev.segment_index] = ev.settle_time_s

    return ZoneRunMetrics(
        zone=zone,
        start_temp_c=start_temp_c,
        iae_normalized_whole_c=whole_stats.iae_normalized_c,
        iae_normalized_by_segment=iae_by_segment,
        ramp_mean_error_c=ramp_mean,
        ramp_worst_error_c=ramp_worst,
        dwell_entry_overshoot_peak_c=overshoot_peak,
        dwell_entry_time_to_peak_s=time_to_peak,
        dwell_steady_state_offset_c=dwell_offset,
        settle_time_s=settle_time,
    )


#: Convention for the recommended ambient-confound protocol (PID_EXPANSION_PLAN.md
#: "A/B campaign ambient-confound protocol"): segment 0 of the profile under
#: test is a stabilisation hold (ramp to a fixed setpoint, dwell until every
#: zone has settled), and the segments that actually get scored -- the ones
#: the plan's numbers are about -- start at this index. Callers that use the
#: stabilised protocol pass ``min_segment_index=STABILIZATION_SEGMENT_INDEX``
#: to :func:`compute_run_metrics` / :func:`fit_start_temp_sensitivity`; callers
#: still on the unstabilised protocol pass nothing (default 0, unchanged
#: behaviour).
#:
#: IMPORTED, not redefined: the value is owned by profile_stabilization.py
#: (the module that actually inserts the hold, so it is the one place the
#: "exactly one segment" structural fact lives). Keeping a second, locally
#: defined ``= 1`` here is exactly the class of bug this repo has shipped
#: repeatedly -- a producer and a consumer that each hard-code the same
#: number and can silently drift apart when one side changes. See
#: ``resolve_min_segment_index`` below for how the runner and this module
#: are kept from disagreeing about which convention any ONE capture used.
STABILIZATION_SEGMENT_INDEX = ps.STABILIZATION_SEGMENT_INDEX


def _read_capture_meta(path: str) -> Optional[dict]:
    """Read the ``{"meta": {...}}`` header line ``run_queue.py`` writes as
    the FIRST line of every capture it opens (see ``run_entry``'s
    ``_write_capture_meta``) -- records whether that run got the
    stabilisation hold and which ``min_segment_index`` the runner therefore
    expects analysis to use. Returns ``None`` (not an error) for a capture
    with no such line -- an older capture, one written by something other
    than ``run_queue.py``, or an empty/unreadable path -- since the absence
    of the marker is meaningful (means "unknown convention, do not assume
    stabilised") rather than a failure to report."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            first_line = fh.readline()
    except OSError:
        return None
    first_line = first_line.strip()
    if not first_line:
        return None
    try:
        obj = json.loads(first_line)
    except json.JSONDecodeError:
        return None
    if not isinstance(obj, dict):
        return None
    meta = obj.get("meta")
    return meta if isinstance(meta, dict) else None


class StabilizationMismatchError(ValueError):
    """Raised by :func:`resolve_min_segment_index` when the scoring window
    the runner recorded and the scoring window analysis was told (or would
    otherwise infer) to use CANNOT be reconciled -- either because two
    captures being compared disagree with each other, or because an
    explicit ``--min-segment-index``/``min_segment_index=`` override
    conflicts with what a capture's own meta line recorded. This is the
    loud failure TASK 1 calls for: the alternative is a silently
    reintroduced ambient-start confound that nothing would report."""


def resolve_min_segment_index(
    path_a: str, path_b: Optional[str] = None, explicit: Optional[int] = None,
) -> int:
    """THE PRODUCER/CONSUMER AGREEMENT POINT for TASK 1's default-on
    stabilisation hold. ``run_queue.py`` (the producer) records, as the
    first line of every capture it writes, whether that run's profile had a
    stabilisation hold prepended and, if so, the ``min_segment_index`` the
    scored window must start at (``STABILIZATION_SEGMENT_INDEX`` -- see
    ``profile_stabilization.py``, the single source of truth for that
    number). This function (the consumer side) reads that meta line back
    out of ``path_a`` and (when comparing a pair) ``path_b``, and:

      * if both captures recorded a convention and they DISAGREE, raises
        :class:`StabilizationMismatchError` -- comparing them would silently
        reintroduce exactly the ambient-start confound the hold exists to
        prevent, and nothing else would say so.
      * if ``explicit`` is given (the CLI's ``--min-segment-index``, or a
        caller passing ``min_segment_index=`` directly) and it conflicts
        with a recorded convention, ALSO raises -- an explicit override is
        the documented escape hatch for a deliberate one-off, not a way to
        silently score a stabilised capture from its ambient segment 0 (or
        vice versa) without comment.
      * otherwise returns whichever convention is available: the explicit
        value if given and uncontested, else the value recorded in either
        capture's meta line, else ``0`` (legacy behaviour, unchanged, for
        captures written before this convention existed)."""
    meta_a = _read_capture_meta(path_a)
    meta_b = _read_capture_meta(path_b) if path_b is not None else None
    idx_a = meta_a.get("min_segment_index") if meta_a else None
    idx_b = meta_b.get("min_segment_index") if meta_b else None

    if idx_a is not None and idx_b is not None and int(idx_a) != int(idx_b):
        raise StabilizationMismatchError(
            f"{path_a!r} was captured with min_segment_index={idx_a!r} but {path_b!r} "
            f"was captured with min_segment_index={idx_b!r} -- these two runs used "
            "different stabilisation conventions (one had the hold prepended, or a "
            "different scored-window start, than the other) and cannot be honestly "
            "compared. Re-run one of them under the same convention, or pass an explicit "
            "--min-segment-index only if you are certain that is the right thing to score."
        )

    detected = idx_a if idx_a is not None else idx_b

    if explicit is not None:
        if detected is not None and int(explicit) != int(detected):
            raise StabilizationMismatchError(
                f"--min-segment-index={explicit} was given explicitly, but the capture's "
                f"own recorded convention is min_segment_index={detected!r} -- the runner "
                "and the analysis must agree. Drop the override to use the recorded value, "
                "or confirm you genuinely intend to score a different window than the run "
                "was captured for."
            )
        return int(explicit)

    return int(detected) if detected is not None else 0


def _first_valid_start_temp(rows: Sequence[la.PollRow], zone: int, min_segment_index: int = 0) -> float:
    """The zone's starting temperature: the first row's ``actual_c`` UNLESS
    that row is invalid, in which case the first genuinely valid row's value
    is used instead.

    2026-09-02f: real HTTP captures (e.g. the noise-floor repeat set) often
    have ``actual_valid=false`` on row 0 -- the poll landed before the first
    thermocouple read completed. ``log_analysis.PollRow``'s own docstring
    documents the firmware's convention for this: ``actual = z->actual_valid
    ? z->actual_c : 0.0f`` -- an invalid reading is carried as a literal
    ``0.0`` placeholder, not a real temperature. Blindly taking ``rows[0]``
    (the previous behaviour) silently treated that placeholder as "the kiln
    started at 0.0C", which is impossible and, worse, made every affected
    run look identical to every other affected run on the confound gate --
    exactly the kind of undetected confound this module exists to catch.
    ``math.nan`` is returned (matching this module's existing "no start
    temp available" convention) if every row for this zone is invalid or the
    zone never appears.

    See ``START_TEMP_METRIC_NOTE`` -- this is the PER-ZONE figure the
    CONFOUND_THRESHOLD_C gate uses, deliberately not the same quantity as
    noise_floor.extract_start_conditions's all-channel mean.

    ``min_segment_index`` -- when scoring only segments >= a stabilisation
    hold (see ``STABILIZATION_SEGMENT_INDEX``), this returns the start
    temperature AT THE START OF THE SCORED WINDOW (the stabilised
    temperature, e.g. ~48C), not the room-ambient temperature the run
    physically began at. That is deliberate: it is what the confound gate
    and sensitivity fit should be comparing across arms once the ambient
    start is no longer what feeds the score."""
    for r in rows:
        if r.segment_index < min_segment_index:
            continue
        s = r.zones.get(zone)
        if s is None:
            continue
        if s.actual_c != 0.0 and not math.isnan(s.actual_c):
            return s.actual_c
    return math.nan


def compute_run_metrics(
    rows: Sequence[la.PollRow], band_c: float = 1.0, min_segment_index: int = 0,
) -> dict:
    """All zones' metrics for a single (already-selected) run.

    ``min_segment_index``: see :func:`compute_zone_metrics` and
    ``STABILIZATION_SEGMENT_INDEX``."""
    zones = la.zones_in_rows(rows)
    starts = {z: _first_valid_start_temp(rows, z, min_segment_index=min_segment_index) for z in zones}
    out = {}
    for z in zones:
        m = compute_zone_metrics(
            rows, z, starts.get(z, math.nan), band_c=band_c, min_segment_index=min_segment_index,
        )
        if m is not None:
            out[z] = m
    return out


# ---------------------------------------------------------------------------
# Loading either capture shape, and picking the run
# ---------------------------------------------------------------------------

def load_run(path: str, run_index: Optional[int] = None) -> list:
    """Load an HTTP-capture JSONL and return ONE run's rows. HTTP captures
    are the only source this module accepts (that is the shape the fuzzy A/B
    runs are being recorded in); the classic ``HH:MM:SS {...}`` poll-capture
    shape stays log_analysis.py's own ``compare`` command's job.

    2026-09-02: this used to always take the file's most recent run, no
    questions asked. That is exactly how a telemetry poller left running
    across a kiln cooldown produced a two-run
    ``p7_oldmatrix_http.jsonl``, and an A/B compare silently took the second
    run from BOTH sides -- comparing it against itself. The output looked
    entirely plausible (matched start temps, near-identical metrics, tidy
    verdicts) and would have been believed. Now: if the file holds more than
    one run that actually carries zone data, this raises
    ``log_analysis.MultiRunError`` (naming how many runs were found and their
    start times/temps) unless ``run_index`` says which one to use. See
    ``split()`` below for splitting a multi-run capture into one file per
    run first.
    """
    all_rows = hc.poll_rows(path)
    if not all_rows:
        return []
    rows, _n_runs, _used = la.select_run(all_rows, path, run_index=run_index)
    return rows


# ---------------------------------------------------------------------------
# THE SHORT/TRUNCATED-ARM GATE.
#
# compare_runs used to compute every metric from whatever rows happened to
# be in a capture file, with no regard for whether the run that produced it
# ever finished. A campaign killed mid-arm (it has happened twice in one
# day), a runner crash, a board reboot, or a full disk all leave behind a
# capture file that EXISTS and PARSES -- fuzzy_ab_analyze.py's own
# file-existence check sees nothing wrong -- but stopped partway through the
# profile. Comparing that against a complete arm produces a large apparent
# difference that is pure artifact (a partial ramp compared against a full
# profile), and the project's >=3-zone decision rule would happily call it
# DISTINGUISHABLE. That is worse than no analysis at all.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class ArmCompleteness:
    path: str
    complete: bool
    reason: str
    final_state: Optional[str]        # the capture's own last row's exec.state, or None
    state_file_status: Optional[str]  # "completed"/"in_progress"/"pending", or None if no entry found
    state_file_path: Optional[str]    # which <prefix>_state.json this came from, if any


def _find_state_file_status(path: str) -> tuple:
    """Best-effort: scan the same directory as ``path`` for any
    ``run_queue.py`` campaign state file (``*_state.json``) whose
    ``entries`` list records THIS capture, and return
    ``(status, state_file_path)`` -- e.g. ``("in_progress",
    ".../fuzzy_ab_20260904d_state.json")``.

    Matched by BASENAME, not full-path resolution: a state file's
    ``entry["log_path"]`` is recorded relative to whatever directory
    ``run_queue.py`` was invoked FROM (typically ``tools/PcTools/scripts``),
    not relative to the state file's own directory -- resolving it as if it
    were would silently point at the wrong file. Every capture in a given
    campaign has a distinctive, campaign-specific basename (the whole point
    of the ``<prefix>_<label>_run<N>.jsonl`` naming convention), so basename
    matching within one directory is unambiguous in practice.

    Returns ``(None, None)`` -- not an error -- when no directory-mate state
    file has a matching entry: an older capture, one made by hand, or one
    whose campaign used a different directory layout. Absence of a state
    file must never block judging the capture on its own merits; see
    :func:`capture_completeness`."""
    directory = os.path.dirname(os.path.abspath(path)) or "."
    target_name = os.path.basename(path)
    try:
        candidates = sorted(f for f in os.listdir(directory) if f.endswith("_state.json"))
    except OSError:
        return None, None
    for fname in candidates:
        state_path = os.path.join(directory, fname)
        try:
            with open(state_path, "r", encoding="utf-8") as fh:
                state = json.load(fh)
        except (OSError, ValueError):
            continue
        if not isinstance(state, dict):
            continue
        for entry in state.get("entries") or []:
            if not isinstance(entry, dict):
                continue
            log_path = entry.get("log_path")
            if log_path and os.path.basename(log_path) == target_name:
                return entry.get("status"), state_path
    return None, None


def capture_completeness(path: str, rows: Optional[Sequence] = None) -> ArmCompleteness:
    """Is this arm's capture COMPLETE, or SHORT/TRUNCATED?

    TWO SIGNALS, in priority order:

      1. THE CAPTURE'S OWN LAST RECORDED ``exec.state`` -- terminal iff it
         is ``"done"`` or ``"faulted"`` (``run_queue.TERMINAL_STATES``, the
         same signal ``run_queue.py`` itself uses internally
         (``_capture_run_reached_terminal_state``) to tell a genuinely
         partial capture from a complete one). This is always available
         once the file has any rows at all, and needs no other file to
         exist -- it is what lets a capture with no matching state file
         (an older run, a hand-made file, or a different directory layout)
         still be judged. This is how the six checked-in
         ``easeoff_ab_20260904_*`` arms, which predate this campaign's
         ``*_state.json`` convention, are judged.
      2. THE RUNNER'S OWN ``*_state.json`` entry for this ``log_path``, when
         one exists in the same directory (see ``_find_state_file_status``).
         Normally the MORE authoritative signal -- it is the runner's own
         bookkeeping of whether it considers the arm done -- but it can go
         stale: a crash between the run finishing and the state file being
         rewritten leaves a ``"pending"``/``"in_progress"`` entry next to a
         capture that is, on its own data, fully complete (see
         ``run_queue.py``'s own RESUME COMPLETION RECOVERY handling of
         exactly this).

    RECONCILIATION:
      * ``"pending"``/``"in_progress"`` in the state file is trusted on its
        own: a run genuinely still running (or never started -- exactly
        arm B1 of the live ``fuzzy_ab_20260904d`` campaign as of this
        writing) cannot be complete no matter how far its last polled row
        happens to look.
      * ``"completed"`` in the state file is CROSS-CHECKED against the
        capture's own last ``exec.state``. Agreement is the strongest
        evidence available. Disagreement (state file says completed, but
        the capture's own last row is not terminal) means the state file
        entry is STALE -- the capture's own data wins, and the mismatch is
        reported loudly rather than trusting an unverifiable external claim
        over the data actually in hand.
      * No matching state-file entry at all: judged purely from the
        capture's own last ``exec.state`` (signal 1).

    ``rows``, if given, must be ``load_run(path)``'s own return value (saves
    a re-parse -- ``compare_runs`` already loaded both arms). If not given,
    this function loads the file itself so it can be used standalone."""
    if rows is None:
        try:
            rows = load_run(path)
        except (FileNotFoundError, OSError, la.MultiRunError, IndexError):
            rows = []

    final_state = rows[-1].state if rows else None
    capture_terminal = bool(final_state) and final_state.lower() in rq.TERMINAL_STATES

    status, state_path = _find_state_file_status(path)

    if status in ("pending", "in_progress"):
        return ArmCompleteness(
            path=path, complete=False,
            reason=f"state file {state_path!r} records this arm as {status!r} -- not yet "
                   "finished, cannot be treated as a complete run",
            final_state=final_state, state_file_status=status, state_file_path=state_path,
        )
    if status == "completed":
        if capture_terminal:
            return ArmCompleteness(
                path=path, complete=True,
                reason=f"state file {state_path!r} records 'completed' and the capture's own "
                       f"last exec.state={final_state!r} is terminal -- agree",
                final_state=final_state, state_file_status=status, state_file_path=state_path,
            )
        return ArmCompleteness(
            path=path, complete=False,
            reason=f"STALE STATE FILE: {state_path!r} records this arm as 'completed' but the "
                   f"capture's own last exec.state={final_state!r} is NOT terminal -- trusting "
                   "the capture's own data over the (apparently stale) state-file claim and "
                   "treating this arm as INCOMPLETE",
            final_state=final_state, state_file_status=status, state_file_path=state_path,
        )
    # No state-file entry found for this path -- judge from the capture alone.
    if not rows:
        return ArmCompleteness(
            path=path, complete=False,
            reason="no parseable rows and no state file entry found -- cannot be judged complete",
            final_state=None, state_file_status=None, state_file_path=None,
        )
    if capture_terminal:
        return ArmCompleteness(
            path=path, complete=True,
            reason=f"no state file entry found; capture's own last exec.state={final_state!r} "
                   "is terminal",
            final_state=final_state, state_file_status=None, state_file_path=None,
        )
    return ArmCompleteness(
        path=path, complete=False,
        reason=f"no state file entry found; capture's own last exec.state={final_state!r} is "
               "NOT terminal -- this looks like a SHORT/TRUNCATED capture (campaign killed "
               "mid-arm, runner crashed, board rebooted, disk full)",
        final_state=final_state, state_file_status=None, state_file_path=None,
    )


# ---------------------------------------------------------------------------
# A/B comparison + the honesty gate
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class MetricComparison:
    zone: int
    metric: str
    segment: Optional[int]
    a: Optional[float]
    b: Optional[float]
    delta: Optional[float]  # b - a
    verdict: str  # "REFUSED: ..." or "PROVISIONAL: ..." or "n/a"
    floor_c: Optional[float] = None
    # True: |delta| >= floor_c (DISTINGUISHABLE). False: |delta| < floor_c
    # (INDISTINGUISHABLE). None: floor unknown, distinguishability cannot be
    # assessed -- NOT the same thing as "distinguishable is false".
    distinguishable: Optional[bool] = None
    # "A", "B", or "tie" -- which run had the lower (better) value, when a
    # verdict was reached. None when no verdict was reached (n/a, REFUSED).
    # Used by summarize_multiplicity to detect a same-metric/same-direction
    # pattern across zones/segments; also lets format_compare_text and any
    # caller avoid parsing the verdict string for this.
    better: Optional[str] = None
    # The repeat-set sample size (artifact entry "n") backing floor_c, when
    # a floor was looked up. PROBLEM 2: the range floor is not comparable
    # across different n (E[range]/sigma grows with n), so this is surfaced
    # on every comparison rather than left implicit.
    floor_n: Optional[int] = None
    # The scale-stable alternative to floor_c: t(.975, n-1) * std_c *
    # sqrt(2), the prediction interval for the difference of two NEW
    # same-config runs. Reported alongside floor_c, never used to compute
    # verdict/distinguishable -- see prediction_interval_floor and the
    # module docstring's PROBLEM 2 section for why (it is WIDER, so
    # defaulting to it would silently lose findings).
    pi_floor_c: Optional[float] = None
    # What distinguishable WOULD be if pi_floor_c were used instead of
    # floor_c. None when pi_floor_c is unavailable or no verdict was
    # reached. Purely informational -- never overrides `distinguishable`.
    pi_distinguishable: Optional[bool] = None
    # Only populated for SENSITIVITY_METRIC when a start-temp sensitivity fit
    # was available (see fit_start_temp_sensitivity / compare_runs). Never
    # changes verdict/distinguishable -- purely explanatory.
    start_temp_adjustment: Optional[dict] = None


#: t-distribution two-sided 97.5th-percentile critical values, keyed by
#: degrees of freedom (df = n - 1). Small, fixed table -- this module has no
#: scipy dependency and only ever needs df in the single digits (repeat
#: campaigns run overnight, not for weeks). Values from standard t tables.
_T_975_TABLE = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
    8: 2.306, 9: 2.262, 10: 2.228, 15: 2.131, 20: 2.086, 25: 2.060, 30: 2.042,
}


def _t_975(df: int) -> Optional[float]:
    """t(.975, df). Exact for the tabulated df; linearly interpolated
    between the nearest tabulated points otherwise; the large-sample normal
    approximation (1.96) above df=30. Returns None for df < 1."""
    if df is None or df < 1:
        return None
    if df in _T_975_TABLE:
        return _T_975_TABLE[df]
    if df > 30:
        return 1.96
    keys = sorted(_T_975_TABLE)
    for lo, hi in zip(keys, keys[1:]):
        if lo < df < hi:
            frac = (df - lo) / (hi - lo)
            return _T_975_TABLE[lo] + frac * (_T_975_TABLE[hi] - _T_975_TABLE[lo])
    return 1.96


def prediction_interval_floor(std_c: Optional[float], n: Optional[int]) -> Optional[float]:
    """The scale-stable alternative to the range floor: a 95% prediction
    interval for the DIFFERENCE between two new same-config runs,
    ``t(.975, n-1) * std_c * sqrt(2)``. At n=6 this is about 3.6 sigma
    (t(.975, 5) approx 2.571) versus the range floor's ~2.53 sigma at the
    same n -- wider, on purpose: it directly answers "could two new
    same-config runs differ by this much?" instead of "how spread out were
    the n runs we happened to capture?" (the range's answer, which grows
    with n rather than converging).

    Returns None if std_c or n is unavailable, or n < 2 (need at least 1
    degree of freedom)."""
    if std_c is None or n is None or n < 2:
        return None
    t = _t_975(n - 1)
    if t is None:
        return None
    return t * std_c * math.sqrt(2)


def _cmp(zone: int, metric: str, segment: Optional[int], a: Optional[float], b: Optional[float],
         start_delta_c: float, compare_by_magnitude: bool = True,
         floor_entry: Optional[dict] = None) -> MetricComparison:
    """``compare_by_magnitude`` (formerly named ``smaller_is_better``, which
    was a misnomer): both branches below prefer the run with the SMALLER
    value -- the flag only chooses whether "smaller" is measured on
    ``abs(value)`` (True, the default -- correct for signed error metrics
    like ramp_mean_error_c, where a large negative undershoot should not
    read as "better" than a small positive overshoot) or on the raw value
    (False -- for metrics that are never negative, e.g. durations, where
    abs() is a no-op anyway). No numeric consequence today since every
    False caller passes non-negative durations, but the old name implied a
    real behavioral toggle that does not exist.

    ``floor_entry``, when given, is the noise_floor.json artifact entry dict
    for this (zone, metric, segment) key -- not just the bare floor_c -- so
    this function can also surface floor_n and compute the prediction-
    interval alternative (see prediction_interval_floor)."""
    floor_c = floor_entry.get("noise_floor_c") if floor_entry else None
    floor_n = floor_entry.get("n") if floor_entry else None
    std_c = floor_entry.get("std_c") if floor_entry else None
    pi_floor_c = prediction_interval_floor(std_c, floor_n)

    if a is None or b is None or (isinstance(a, float) and math.isnan(a)) or (isinstance(b, float) and math.isnan(b)):
        return MetricComparison(zone, metric, segment, a, b, None, "n/a: missing data",
                                 floor_c=floor_c, floor_n=floor_n, pi_floor_c=pi_floor_c)
    delta = b - a
    if start_delta_c > CONFOUND_THRESHOLD_C:
        return MetricComparison(
            zone, metric, segment, a, b, delta,
            f"REFUSED: zone {zone} start-temp delta {start_delta_c:.1f}C exceeds the "
            f"{CONFOUND_THRESHOLD_C:.1f}C confound threshold -- cannot attribute this "
            f"difference to fuzzy strength",
            floor_c=floor_c, floor_n=floor_n, pi_floor_c=pi_floor_c, distinguishable=None,
        )
    pi_distinguishable = (abs(delta) >= pi_floor_c) if pi_floor_c is not None else None
    if floor_c is not None and abs(delta) < floor_c:
        return MetricComparison(
            zone, metric, segment, a, b, delta,
            f"INDISTINGUISHABLE: |delta|={abs(delta):.3f} is below the measured noise "
            f"floor {floor_c:.3f} for this (zone, metric, segment) -- not attributable "
            f"to whatever is being compared",
            floor_c=floor_c, floor_n=floor_n, pi_floor_c=pi_floor_c,
            distinguishable=False, pi_distinguishable=pi_distinguishable,
        )
    if compare_by_magnitude:
        better = "B" if abs(b) < abs(a) else ("A" if abs(a) < abs(b) else "tie")
    else:
        better = "B" if b < a else ("A" if a < b else "tie")
    if floor_c is not None:
        floor_note = f"DISTINGUISHABLE: |delta|={abs(delta):.3f} exceeds measured floor {floor_c:.3f}"
        distinguishable = True
    else:
        floor_note = "noise floor unknown -- distinguishability cannot be assessed"
        distinguishable = None
    return MetricComparison(
        zone, metric, segment, a, b, delta,
        f"PROVISIONAL: {better} {'lower-magnitude' if compare_by_magnitude else 'lower'} "
        f"({floor_note} -- not a confirmed result)",
        floor_c=floor_c, floor_n=floor_n, pi_floor_c=pi_floor_c,
        distinguishable=distinguishable, pi_distinguishable=pi_distinguishable, better=better,
    )


# ---------------------------------------------------------------------------
# Start-temperature covariate: sensitivity fit and reporting
#
# See the module docstring's 2026-09-02f section for the honesty framing.
# This section never gates REFUSED/INDISTINGUISHABLE/PROVISIONAL -- it only
# adds supplementary numbers that show how much of a raw delta the known
# start-temp difference could explain.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class StartTempSensitivity:
    zone: int
    n: int
    slope_c_per_c: Optional[float]   # d(iae_normalized_whole_c) / d(start_temp_c)
    intercept: Optional[float]
    r: Optional[float]
    note: str


def _ols(xs: Sequence[float], ys: Sequence[float]) -> Optional[tuple]:
    n = len(xs)
    if n < 2:
        return None
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    sxx = sum((x - mean_x) ** 2 for x in xs)
    if sxx == 0.0:
        return None  # every x identical -- slope undefined, not zero
    sxy = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    slope = sxy / sxx
    intercept = mean_y - slope * mean_x
    return slope, intercept


def _pearson(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    import statistics as _stats
    if len(xs) < 2 or len(xs) != len(ys):
        return None
    try:
        return _stats.correlation(xs, ys)
    except _stats.StatisticsError:
        return None


def fit_start_temp_sensitivity(
    paths: Sequence[str], band_c: float = 1.0,
    run_indices: Optional[Sequence[Optional[int]]] = None,
    min_segment_index: int = 0,
) -> dict:
    """Per-zone OLS slope of ``SENSITIVITY_METRIC`` (iae_normalized_whole_c)
    on start temperature, fit across ``paths``.

    ``min_segment_index``: pass ``STABILIZATION_SEGMENT_INDEX`` to refit this
    against a stabilised-protocol repeat set -- both the x (start temp) and y
    (whole-window IAE) values then come from the scored window only, same as
    :func:`compute_run_metrics`. Do not mix a stabilised repeat set fit at
    ``min_segment_index=0`` (that reintroduces the room-ambient start into the
    x-axis) or an unstabilised set fit at ``min_segment_index=1`` (there is no
    segment 1 to score).

    ASSUMPTIONS -- stated because they matter more than the fit:
      * every path is the SAME controller configuration (same gains/preset/
        profile). This function has no way to verify that; pass a genuine
        same-config repeat set (e.g. a noise-floor artifact's own
        ``generated_from``), never an arbitrary set of captures.
      * the relationship is treated as LINEAR over the observed start-temp
        range only -- not validated, and not to be extrapolated outside that
        range.
      * n < MIN_N_FOR_SENSITIVITY (3): refuses to fit, returns a note
        explaining why, no slope. n as small as 3-6 still makes this a rough
        magnitude, not a precise instrument -- n and r are carried alongside
        the slope everywhere it's used so nothing pretends otherwise.
    """
    if run_indices is None:
        run_indices = [None] * len(paths)
    per_zone_xy: dict = {}
    for path, ridx in zip(paths, run_indices):
        try:
            rows = load_run(path, run_index=ridx)
        except (FileNotFoundError, OSError, la.MultiRunError, IndexError):
            continue
        if not rows:
            continue
        metrics = compute_run_metrics(rows, band_c=band_c, min_segment_index=min_segment_index)
        for z, m in metrics.items():
            if math.isnan(m.start_temp_c) or math.isnan(m.iae_normalized_whole_c):
                continue
            per_zone_xy.setdefault(z, []).append((m.start_temp_c, m.iae_normalized_whole_c))

    out: dict = {}
    for z, pts in per_zone_xy.items():
        n = len(pts)
        if n < MIN_N_FOR_SENSITIVITY:
            out[z] = StartTempSensitivity(
                z, n, None, None, None,
                f"n={n} < MIN_N_FOR_SENSITIVITY={MIN_N_FOR_SENSITIVITY} -- insufficient "
                "data to fit a start-temp sensitivity; no adjustment reported for this zone",
            )
            continue
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        fit = _ols(xs, ys)
        if fit is None:
            out[z] = StartTempSensitivity(
                z, n, None, None, None,
                "start temperatures identical across all repeats for this zone -- slope undefined",
            )
            continue
        slope, intercept = fit
        r = _pearson(xs, ys)
        out[z] = StartTempSensitivity(
            z, n, slope, intercept, r,
            f"OLS fit, n={n} points, over the observed start-temp range "
            f"{min(xs):.2f}-{max(xs):.2f}C only. n this small means the slope's own "
            "precision is not established -- read it as a rough magnitude, not a "
            "calibrated per-degree correction.",
        )
    return out


def _start_temp_adjustment(
    sensitivity: Optional[StartTempSensitivity], start_a: float, start_b: float, raw_delta: float,
) -> Optional[dict]:
    """Decompose ``raw_delta`` (b - a) into the portion the fitted start-temp
    sensitivity would predict from (start_b - start_a), and the residual --
    i.e. regress the outcome on start temperature and report the residual,
    one of the candidate methods this was scoped to consider. Returns
    ``None`` (report nothing) if no usable fit exists for this zone."""
    if sensitivity is None or sensitivity.slope_c_per_c is None:
        return None
    if math.isnan(start_a) or math.isnan(start_b):
        return None
    start_delta = start_b - start_a
    predicted = sensitivity.slope_c_per_c * start_delta
    residual = raw_delta - predicted
    return {
        "sensitivity_n": sensitivity.n,
        "sensitivity_slope_c_per_c": sensitivity.slope_c_per_c,
        "sensitivity_r": sensitivity.r,
        "sensitivity_note": sensitivity.note,
        "start_delta_c": start_delta,
        "raw_delta": raw_delta,
        "predicted_from_start_temp": predicted,
        "residual_after_adjustment": residual,
    }


def summarize_metric_floor_reliability(artifact: Optional[dict]) -> dict:
    """For each metric name present in ``artifact['entries']``, compute the
    ratio of the largest to the smallest measured ``noise_floor_c`` across
    every (zone, segment) entry for that metric. A metric whose own floor
    swings by more than ``FLOOR_RELIABILITY_RATIO`` from one zone/segment to
    the next has an UNSTABLE floor: the floor gating its
    DISTINGUISHABLE/INDISTINGUISHABLE calls is itself noisy, so those calls
    for that metric should be read with extra caution. Returns
    ``{metric: {"n_entries": int, "floor_min": float, "floor_max": float,
    "ratio": float, "reliable": bool}}``; a metric with fewer than 2 floor
    entries reports ``reliable: None`` (not enough data to judge)."""
    if not artifact or not artifact.get("entries"):
        return {}
    by_metric: dict = {}
    for entry in artifact["entries"].values():
        floor = entry.get("noise_floor_c")
        if floor is None:
            continue
        by_metric.setdefault(entry["metric"], []).append(floor)
    out = {}
    for metric, floors in by_metric.items():
        if len(floors) < 2:
            out[metric] = {
                "n_entries": len(floors), "floor_min": floors[0] if floors else None,
                "floor_max": floors[0] if floors else None, "ratio": None, "reliable": None,
            }
            continue
        lo, hi = min(floors), max(floors)
        ratio = (hi / lo) if lo > 0 else math.inf
        out[metric] = {
            "n_entries": len(floors), "floor_min": lo, "floor_max": hi, "ratio": ratio,
            "reliable": ratio <= FLOOR_RELIABILITY_RATIO,
        }
    return out


# ---------------------------------------------------------------------------
# PROBLEM 1: multiplicity. The verdict-per-key report has no correction; this
# section reports the family size, the per-key false-positive rate the range
# floor actually carries (Monte Carlo, since the range statistic has no
# closed form here), and the one pattern the review calls potentially
# actionable -- a same metric, same direction across several keys.
# ---------------------------------------------------------------------------

#: Monte Carlo trial count / seed for _range_floor_false_positive_rate.
#: Fixed and deterministic so the reported rate does not wobble between
#: runs of the same report.
_ALPHA_TRIALS = 20000
_ALPHA_SEED = 20260903

#: The review's estimate of the effective (correlation-adjusted) number of
#: independent tests behind a ~48-key report like this rig's -- not derived
#: from this artifact, an honest range from the review itself. Used only to
#: show that the naive independence-assumed probability below overstates
#: the true familywise risk, not as a precise correction.
EFFECTIVE_FAMILY_SIZE_RANGE = (10, 15)

#: A metric is only flagged as a "consistent pattern" (see
#: summarize_multiplicity) once at least this many DISTINGUISHABLE keys
#: agree on both metric and direction. 3 is a floor, not a target -- it is
#: chosen so a two-zone coincidence cannot pass as a pattern.
CONSISTENT_PATTERN_MIN_KEYS = 3


def _range_floor_false_positive_rate(n: Optional[int], trials: int = _ALPHA_TRIALS,
                                      seed: int = _ALPHA_SEED) -> Optional[float]:
    """Monte Carlo estimate of P(|difference of two NEW same-config draws|
    exceeds the max-minus-min range of n other same-config draws), under a
    normal same-config model. Scale-free (sigma=1 throughout -- the
    probability does not depend on the population's actual sigma). This is
    the per-key false-positive rate the range floor actually carries; it is
    NOT 5%, and it grows with n (see the module docstring's PROBLEM 2).
    Deterministic given (n, trials, seed) so a report's reported rate is
    reproducible. Returns None for n < 2 (no range defined)."""
    if n is None or n < 2:
        return None
    rng = random.Random(seed * 1000 + n)
    hits = 0
    for _ in range(trials):
        sample = [rng.gauss(0.0, 1.0) for _ in range(n)]
        floor = max(sample) - min(sample)
        diff = rng.gauss(0.0, 1.0) - rng.gauss(0.0, 1.0)  # ~ N(0, 2)
        if abs(diff) > floor:
            hits += 1
    return hits / trials


@dataclasses.dataclass
class MultiplicitySummary:
    family_size: int
    n_distinguishable: int
    #: {n: monte-carlo false-positive rate for a range floor built from that n}
    per_n_alpha: dict
    #: sum, over every keyed comparison, of its own per-n false-positive
    #: rate -- the expected NUMBER of spurious DISTINGUISHABLE verdicts in
    #: this report by chance alone, if every comparison were truly null.
    expected_false_positives: Optional[float]
    #: P(at least one spurious DISTINGUISHABLE), assuming every key is an
    #: INDEPENDENT test at its own per-n rate. Overstated -- keys share
    #: zones/segments/underlying physics -- see prob_at_least_one_effective_*.
    prob_at_least_one_independent: Optional[float]
    effective_family_size_low: int
    effective_family_size_high: int
    #: P(at least one spurious DISTINGUISHABLE) recomputed at the review's
    #: estimated effective (correlation-adjusted) family size, using the
    #: false-positive rate for the n most keys actually have.
    prob_at_least_one_effective_low: Optional[float]
    prob_at_least_one_effective_high: Optional[float]
    #: [{"metric", "direction" ("A"/"B"), "n_keys", "zones", "segments"}, ...]
    #: for every metric where >= CONSISTENT_PATTERN_MIN_KEYS DISTINGUISHABLE
    #: keys agree on direction -- the one pattern this module treats as
    #: potentially actionable, sorted by n_keys descending.
    consistent_patterns: list
    note: str


def summarize_multiplicity(comparisons: Sequence[MetricComparison]) -> MultiplicitySummary:
    """PROBLEM 1's fix: report the family size, per-key false-positive rate,
    and familywise risk this comparison never surfaced before, plus the one
    signal the review calls potentially actionable (a same-metric,
    same-direction pattern across multiple keys). Never changes any
    individual comparison's verdict -- purely a summary layer read alongside
    ``report["comparisons"]``."""
    keyed = [c for c in comparisons if c.distinguishable is not None]
    family_size = len(keyed)
    distinguishable = [c for c in keyed if c.distinguishable]

    ns = sorted({c.floor_n for c in keyed if c.floor_n is not None})
    per_n_alpha = {n: _range_floor_false_positive_rate(n) for n in ns}

    expected_fp = None
    prob_indep = None
    if per_n_alpha:
        rates = [per_n_alpha[c.floor_n] for c in keyed if c.floor_n in per_n_alpha]
        expected_fp = sum(rates) if rates else None
        if rates:
            prod = 1.0
            for a in rates:
                prod *= (1.0 - a)
            prob_indep = 1.0 - prod

    dominant_alpha = None
    if ns:
        counts = Counter(c.floor_n for c in keyed if c.floor_n is not None)
        dominant_n = counts.most_common(1)[0][0]
        dominant_alpha = per_n_alpha.get(dominant_n)
    eff_lo, eff_hi = EFFECTIVE_FAMILY_SIZE_RANGE
    prob_eff_lo = prob_eff_hi = None
    if dominant_alpha is not None:
        prob_eff_lo = 1.0 - (1.0 - dominant_alpha) ** eff_lo
        prob_eff_hi = 1.0 - (1.0 - dominant_alpha) ** eff_hi

    by_metric_dir: dict = {}
    for c in distinguishable:
        if c.better not in ("A", "B"):
            continue
        by_metric_dir.setdefault((c.metric, c.better), []).append(c)
    patterns = []
    for (metric, direction), keys in by_metric_dir.items():
        if len(keys) >= CONSISTENT_PATTERN_MIN_KEYS:
            patterns.append({
                "metric": metric, "direction": direction, "n_keys": len(keys),
                "zones": sorted({k.zone for k in keys}),
                "segments": sorted({k.segment for k in keys if k.segment is not None}),
            })
    patterns.sort(key=lambda p: -p["n_keys"])

    note = (
        f"{family_size} (zone, metric, segment) keys evaluated, no multiplicity "
        "correction applied to the raw per-key verdicts above. The range floor's own "
        "per-key false-positive rate is NOT 5% -- it depends on n (see per_n_alpha) "
        "and is roughly 12% at n=6. A SINGLE DISTINGUISHABLE key in a report this size "
        "is close to the expected outcome under pure chance -- read it as PROVISIONAL "
        "AT BEST, not a finding. prob_at_least_one_independent assumes every key is an "
        "independent test and OVERSTATES the true risk (keys share zones, segments, and "
        "underlying physics); prob_at_least_one_effective_low/high use the review's "
        "estimated effective family size of 10-15 independent tests instead -- still "
        "approximate, since the correlation structure between keys is not itself "
        "measured. The one pattern treated as potentially actionable here: the SAME "
        "metric DISTINGUISHABLE in the SAME direction across "
        f">= {CONSISTENT_PATTERN_MIN_KEYS} zones/segments -- see consistent_patterns."
    )

    return MultiplicitySummary(
        family_size=family_size,
        n_distinguishable=len(distinguishable),
        per_n_alpha=per_n_alpha,
        expected_false_positives=expected_fp,
        prob_at_least_one_independent=prob_indep,
        effective_family_size_low=eff_lo,
        effective_family_size_high=eff_hi,
        prob_at_least_one_effective_low=prob_eff_lo,
        prob_at_least_one_effective_high=prob_eff_hi,
        consistent_patterns=patterns,
        note=note,
    )


def compare_runs(
    path_a: str, path_b: str, band_c: float = 1.0,
    run_index_a: Optional[int] = None, run_index_b: Optional[int] = None,
    noise_floor_artifact: Optional[dict] = None,
    sensitivity_paths: Optional[Sequence[str]] = None,
    min_segment_index: Optional[int] = None,
    check_completeness: bool = True,
) -> dict:
    """``check_completeness`` (default True): refuse to compare, exactly as
    the confound gate refuses, when either arm's capture is SHORT or
    TRUNCATED -- see :func:`capture_completeness`. Pass False only for a
    caller that has already established both captures represent complete
    runs by some other means, or a test exercising unrelated logic against a
    deliberately-truncated fixture excerpt; the real campaign entry point
    (``fuzzy_ab_analyze.py``) always leaves this at its default.

    ``sensitivity_paths``, if given, must be a genuine same-configuration
    repeat set (see ``fit_start_temp_sensitivity``'s assumptions) used to
    estimate the start-temp sensitivity reported alongside the whole-run IAE
    comparison. Defaults to ``noise_floor_artifact['generated_from']`` when
    not given explicitly and the artifact carries one -- the noise-floor
    campaign's repeat set is exactly this kind of same-config set by
    construction. Never affects REFUSED/INDISTINGUISHABLE/PROVISIONAL --
    see the module docstring's 2026-09-02f section.

    ``min_segment_index``: defaults to ``None``, which means "figure it out
    from the captures themselves" via :func:`resolve_min_segment_index` --
    ``run_queue.py`` (as of TASK 1, stabilisation-hold-by-default) writes
    the convention each capture used into its own meta line, so a caller no
    longer has to remember to pass ``STABILIZATION_SEGMENT_INDEX`` by hand.
    Pass an explicit int only for the documented escape-hatch case (scoring
    a window other than what the capture recorded); doing so against a
    capture whose meta line disagrees raises
    :class:`StabilizationMismatchError` rather than silently scoring the
    wrong window -- see that function's docstring. Two captures whose OWN
    recorded conventions disagree with EACH OTHER raise the same way even
    with no explicit override, since comparing them at all would silently
    reintroduce the ambient-start confound the hold exists to prevent."""
    try:
        min_segment_index = resolve_min_segment_index(path_a, path_b, explicit=min_segment_index)
    except StabilizationMismatchError as exc:
        return {"error": str(exc)}
    try:
        rows_a = load_run(path_a, run_index=run_index_a)
        rows_b = load_run(path_b, run_index=run_index_b)
    except (FileNotFoundError, OSError, la.MultiRunError, IndexError) as exc:
        return {"error": str(exc)}
    if not rows_a or not rows_b:
        return {"error": "one or both HTTP captures had no parseable rows"}

    completeness_a = capture_completeness(path_a, rows_a) if check_completeness else None
    completeness_b = capture_completeness(path_b, rows_b) if check_completeness else None
    if check_completeness and (not completeness_a.complete or not completeness_b.complete):
        incomplete = [c for c in (completeness_a, completeness_b) if not c.complete]
        complete_side = [c for c in (completeness_a, completeness_b) if c.complete]
        asym_note = ""
        if complete_side:
            asym_note = (
                f"THE DANGEROUS ASYMMETRIC CASE: the other arm ({complete_side[0].path!r}) IS "
                "complete -- comparing it against a truncated arm would look like a valid pair "
                "and produce a confident but artifactual verdict (a partial ramp compared "
                "against a full profile). "
            )
        return {
            "error": (
                "INCOMPLETE/VOID: at least one arm's capture is short or truncated -- "
                "refusing to compare. " + asym_note +
                "; ".join(f"{c.path!r}: {c.reason}" for c in incomplete)
            ),
            "incomplete_arms": {
                "a": None if completeness_a.complete else dataclasses.asdict(completeness_a),
                "b": None if completeness_b.complete else dataclasses.asdict(completeness_b),
            },
        }

    metrics_a = compute_run_metrics(rows_a, band_c=band_c, min_segment_index=min_segment_index)
    metrics_b = compute_run_metrics(rows_b, band_c=band_c, min_segment_index=min_segment_index)
    zones = sorted(set(metrics_a) & set(metrics_b))

    start_deltas = {
        z: abs(metrics_a[z].start_temp_c - metrics_b[z].start_temp_c)
        for z in zones
        if not math.isnan(metrics_a[z].start_temp_c) and not math.isnan(metrics_b[z].start_temp_c)
    }

    def _floor_entry(z: int, metric: str, seg: Optional[int]) -> Optional[dict]:
        """The full noise_floor.json artifact entry (not just noise_floor_c)
        for this key, so _cmp can also surface floor_n and compute the
        prediction-interval alternative. Key format matches
        noise_floor._key_str exactly (verified against noise_floor.py, not
        imported from it -- that module is out of scope for this pass)."""
        if not noise_floor_artifact:
            return None
        key = f"z{z}:{metric}:{'whole' if seg is None else seg}"
        return noise_floor_artifact.get("entries", {}).get(key)

    if sensitivity_paths is None and noise_floor_artifact:
        sensitivity_paths = noise_floor_artifact.get("generated_from") or None
    sensitivity: dict = {}
    if sensitivity_paths and len(sensitivity_paths) >= MIN_N_FOR_SENSITIVITY:
        try:
            sensitivity = fit_start_temp_sensitivity(
                sensitivity_paths, band_c=band_c, min_segment_index=min_segment_index,
            )
        except Exception:
            sensitivity = {}

    comparisons: list = []
    for z in zones:
        ma, mb = metrics_a[z], metrics_b[z]
        sd = start_deltas.get(z, math.inf)  # unknown start temp -> maximally distrustful
        whole_cmp = _cmp(z, "iae_normalized_whole_c", None, ma.iae_normalized_whole_c, mb.iae_normalized_whole_c, sd, floor_entry=_floor_entry(z, "iae_normalized_whole_c", None))
        if whole_cmp.delta is not None:
            whole_cmp.start_temp_adjustment = _start_temp_adjustment(
                sensitivity.get(z), ma.start_temp_c, mb.start_temp_c, whole_cmp.delta,
            )
        comparisons.append(whole_cmp)
        segs = sorted(set(ma.iae_normalized_by_segment) & set(mb.iae_normalized_by_segment))
        for seg in segs:
            comparisons.append(_cmp(z, "iae_normalized_c", seg, ma.iae_normalized_by_segment.get(seg), mb.iae_normalized_by_segment.get(seg), sd, floor_entry=_floor_entry(z, "iae_normalized_c", seg)))
        segs = sorted(set(ma.ramp_mean_error_c) & set(mb.ramp_mean_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_mean_error_c", seg, ma.ramp_mean_error_c.get(seg), mb.ramp_mean_error_c.get(seg), sd, floor_entry=_floor_entry(z, "ramp_mean_error_c", seg)))
        segs = sorted(set(ma.ramp_worst_error_c) & set(mb.ramp_worst_error_c))
        for seg in segs:
            comparisons.append(_cmp(z, "ramp_worst_error_c", seg, ma.ramp_worst_error_c.get(seg), mb.ramp_worst_error_c.get(seg), sd, floor_entry=_floor_entry(z, "ramp_worst_error_c", seg)))
        segs = sorted(set(ma.dwell_entry_overshoot_peak_c) & set(mb.dwell_entry_overshoot_peak_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_entry_overshoot_peak_c", seg, ma.dwell_entry_overshoot_peak_c.get(seg), mb.dwell_entry_overshoot_peak_c.get(seg), sd, floor_entry=_floor_entry(z, "dwell_entry_overshoot_peak_c", seg)))
        segs = sorted(set(ma.dwell_entry_time_to_peak_s) & set(mb.dwell_entry_time_to_peak_s))
        for seg in segs:
            a_v, b_v = ma.dwell_entry_time_to_peak_s.get(seg), mb.dwell_entry_time_to_peak_s.get(seg)
            comparisons.append(_cmp(z, "dwell_entry_time_to_peak_s", seg, a_v, b_v, sd, compare_by_magnitude=False, floor_entry=_floor_entry(z, "dwell_entry_time_to_peak_s", seg)))
        segs = sorted(set(ma.dwell_steady_state_offset_c) & set(mb.dwell_steady_state_offset_c))
        for seg in segs:
            comparisons.append(_cmp(z, "dwell_steady_state_offset_c", seg, ma.dwell_steady_state_offset_c.get(seg), mb.dwell_steady_state_offset_c.get(seg), sd, floor_entry=_floor_entry(z, "dwell_steady_state_offset_c", seg)))
        segs = sorted(set(ma.settle_time_s) & set(mb.settle_time_s))
        for seg in segs:
            a_v, b_v = ma.settle_time_s.get(seg), mb.settle_time_s.get(seg)
            comparisons.append(_cmp(z, "settle_time_s", seg, a_v, b_v, sd, compare_by_magnitude=False, floor_entry=_floor_entry(z, "settle_time_s", seg)))

    return {
        "path_a": path_a, "path_b": path_b,
        "noise_floor_known": bool(noise_floor_artifact and noise_floor_artifact.get("entries")),
        "zones": zones,
        "start_temps_a": {z: metrics_a[z].start_temp_c for z in zones},
        "start_temps_b": {z: metrics_b[z].start_temp_c for z in zones},
        "start_temp_deltas_c": start_deltas,
        "metrics_a": metrics_a,
        "metrics_b": metrics_b,
        "comparisons": comparisons,
        "n_rows_a": len(rows_a), "n_rows_b": len(rows_b),
        "start_temp_sensitivity": sensitivity,
        "metric_floor_reliability": summarize_metric_floor_reliability(noise_floor_artifact),
        "multiplicity": summarize_multiplicity(comparisons),
        "start_temp_metric_note": START_TEMP_METRIC_NOTE,
    }


# ---------------------------------------------------------------------------
# Text / JSON rendering
# ---------------------------------------------------------------------------

def format_single_run_text(path: str, metrics: dict) -> str:
    lines = [f"run: {path}"]
    for z in sorted(metrics):
        m = metrics[z]
        lines.append(f"  zone {z}  start_temp={m.start_temp_c:.2f}C")
        lines.append(f"    iae_normalized whole-run: {m.iae_normalized_whole_c:.3f}C")
        for seg in sorted(m.iae_normalized_by_segment):
            lines.append(f"    seg{seg}: iae_norm={m.iae_normalized_by_segment[seg]:.3f}C")
            if seg in m.ramp_mean_error_c:
                lines.append(
                    f"      ramp: mean_err={m.ramp_mean_error_c[seg]:+.2f}C "
                    f"worst_err={m.ramp_worst_error_c[seg]:.2f}C"
                )
            if seg in m.dwell_entry_overshoot_peak_c:
                peak = m.dwell_entry_overshoot_peak_c[seg]
                ttp = m.dwell_entry_time_to_peak_s.get(seg)
                offset = m.dwell_steady_state_offset_c.get(seg)
                settle = m.settle_time_s.get(seg)
                lines.append(
                    f"      dwell-entry: overshoot_peak="
                    f"{'n/a' if peak is None else f'{peak:+.2f}C'} "
                    f"time_to_peak={'n/a' if ttp is None else f'{ttp:.0f}s'} "
                    f"steady_offset={'n/a' if offset is None else f'{offset:+.2f}C'} "
                    f"settle_time={'n/a' if settle is None else f'{settle:.0f}s'}"
                )
    return "\n".join(lines)


def format_compare_text(report: dict) -> str:
    if "error" in report:
        return f"error: {report['error']}"
    lines = [
        f"A/B compare: A={report['path_a']}  B={report['path_b']}",
        NOISE_FLOOR_KNOWN_NOTE if report.get("noise_floor_known") else NOISE_FLOOR_NOTE,
        report.get("start_temp_metric_note", START_TEMP_METRIC_NOTE),
        "",
    ]
    for z in report["zones"]:
        d = report["start_temp_deltas_c"].get(z, float("nan"))
        lines.append(
            f"zone {z}: start_temp A={report['start_temps_a'][z]:.2f}C "
            f"B={report['start_temps_b'][z]:.2f}C delta={d:.2f}C"
            + (f"  *** EXCEEDS {CONFOUND_THRESHOLD_C:.1f}C CONFOUND THRESHOLD ***" if d > CONFOUND_THRESHOLD_C else "")
        )

    mult = report.get("multiplicity")
    if mult is not None:
        lines.append("")
        lines.append(
            f"MULTIPLICITY: {mult.family_size} keys evaluated, {mult.n_distinguishable} DISTINGUISHABLE, "
            "no correction applied to the per-key verdicts below."
        )
        if mult.per_n_alpha:
            alpha_s = ", ".join(f"n={n}: {a*100:.1f}%" for n, a in sorted(mult.per_n_alpha.items()))
            lines.append(f"  per-key false-positive rate of the range floor (Monte Carlo): {alpha_s}")
        if mult.expected_false_positives is not None:
            lines.append(f"  expected spurious DISTINGUISHABLE keys by chance alone: {mult.expected_false_positives:.2f}")
        if mult.prob_at_least_one_independent is not None:
            lines.append(
                f"  P(>=1 spurious DISTINGUISHABLE), independence assumed (overstated): "
                f"{mult.prob_at_least_one_independent*100:.1f}%"
            )
        if mult.prob_at_least_one_effective_low is not None:
            lines.append(
                f"  P(>=1 spurious DISTINGUISHABLE), effective family size "
                f"{mult.effective_family_size_low}-{mult.effective_family_size_high} (correlation-adjusted): "
                f"{mult.prob_at_least_one_effective_low*100:.1f}%-{mult.prob_at_least_one_effective_high*100:.1f}%"
            )
        if mult.consistent_patterns:
            lines.append("  CONSISTENT PATTERN(S) -- same metric, same direction, across multiple keys (potentially actionable):")
            for p in mult.consistent_patterns:
                lines.append(
                    f"    {p['metric']:30s} {p['direction']} better in {p['n_keys']} keys "
                    f"(zones {p['zones']}, segments {p['segments']})"
                )
        else:
            lines.append("  no consistent same-metric/same-direction pattern found -- treat any lone DISTINGUISHABLE key with caution")
        lines.append(f"  {mult.note}")
        lines.append("")

    reliability = report.get("metric_floor_reliability") or {}
    if reliability:
        lines.append("metric floor reliability (per-metric spread of the measured floor across zones/segments):")
        for metric in sorted(reliability):
            r = reliability[metric]
            if r["reliable"] is None:
                lines.append(f"  {metric:30s} n_entries={r['n_entries']} (need >=2 to judge)")
            else:
                tag = "reliable" if r["reliable"] else "UNSTABLE -- treat distinguishability calls for this metric with caution"
                lines.append(
                    f"  {metric:30s} floor {r['floor_min']:.3f}-{r['floor_max']:.3f}C "
                    f"(ratio {r['ratio']:.1f}x) -> {tag}"
                )
        lines.append("")

    lines.append("")
    for c in report["comparisons"]:
        seg = f"seg{c.segment}" if c.segment is not None else "whole-run"
        a_s = "n/a" if c.a is None else f"{c.a:+.3f}"
        b_s = "n/a" if c.b is None else f"{c.b:+.3f}"
        lines.append(f"  z{c.zone} {seg:10s} {c.metric:30s} A={a_s:>8s} B={b_s:>8s}  {c.verdict}")
        adj = c.start_temp_adjustment
        if adj:
            lines.append(
                f"      start-temp sensitivity (n={adj['sensitivity_n']}, "
                f"slope={adj['sensitivity_slope_c_per_c']:+.4f}C/C, r={adj['sensitivity_r']!r}): "
                f"raw_delta={adj['raw_delta']:+.3f} predicted_from_start_delta={adj['predicted_from_start_temp']:+.3f} "
                f"residual={adj['residual_after_adjustment']:+.3f} -- residual is the part of the delta the "
                "fitted start-temp sensitivity does NOT explain; this does not change the verdict above"
            )
            lines.append(f"      ({adj['sensitivity_note']})")
    return "\n".join(lines)


def _jsonable(obj):
    if dataclasses.is_dataclass(obj) and not isinstance(obj, type):
        return {k: _jsonable(v) for k, v in dataclasses.asdict(obj).items()}
    if isinstance(obj, dict):
        return {str(k): _jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_jsonable(v) for v in obj]
    if isinstance(obj, float) and math.isnan(obj):
        return None
    return obj


def compare_report_to_json(report: dict) -> str:
    return json.dumps(_jsonable(report), indent=2)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-pid-ab-compare",
        description="Compare two HTTP-capture firing runs (e.g. fuzzy=0 vs fuzzy=50) against the PID_EXPANSION_PLAN.md SS2 metric set.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_single = sub.add_parser("run", help="report per-zone metrics for a single HTTP capture")
    p_single.add_argument("path")
    p_single.add_argument("--band", type=float, default=1.0)
    p_single.add_argument("--run", type=int, default=None, help="explicit run index, required if the capture holds more than one run")
    p_single.add_argument("--json", action="store_true")
    p_single.add_argument(
        "--min-segment-index", dest="min_segment_index", type=int, default=None,
        help="score only segments >= this index (default: whatever run_queue.py recorded "
             "for this capture, via its meta line -- STABILIZATION_SEGMENT_INDEX=1 for a "
             "stabilised run, 0 otherwise). Pass explicitly only for the documented "
             "escape-hatch case; it is refused if it conflicts with what the capture itself "
             "recorded.")

    p_cmp = sub.add_parser("compare", help="A/B compare two HTTP captures, with the start-temp confound gate")
    p_cmp.add_argument("path_a")
    p_cmp.add_argument("path_b")
    p_cmp.add_argument("--band", type=float, default=1.0)
    p_cmp.add_argument("--run-a", dest="run_a", type=int, default=None, help="explicit run index for path_a, required if it holds more than one run")
    p_cmp.add_argument("--run-b", dest="run_b", type=int, default=None, help="explicit run index for path_b, required if it holds more than one run")
    p_cmp.add_argument("--json", action="store_true")
    p_cmp.add_argument("--noise-floor", dest="noise_floor_path", default=nf.DEFAULT_ARTIFACT_PATH,
                        help="path to the noise_floor.json artifact (default: the checked-in one); "
                             "pass 'none' to compare without it, as if it never existed")
    p_cmp.add_argument("--sensitivity-from", dest="sensitivity_from", default=None, nargs="+",
                        help="paths to a genuine same-configuration repeat set used to fit the "
                             "start-temp sensitivity reported alongside the whole-run IAE comparison "
                             "(default: the noise-floor artifact's own generated_from list, if any)")
    p_cmp.add_argument(
        "--min-segment-index", dest="min_segment_index", type=int, default=None,
        help="score only segments >= this index (default: whatever run_queue.py recorded for "
             "these captures, via each one's meta line -- STABILIZATION_SEGMENT_INDEX=1 for a "
             "stabilised pair, 0 otherwise). Refused if path_a and path_b recorded different "
             "conventions, or if this override conflicts with what either recorded -- the "
             "runner and the analysis must agree.")
    p_cmp.add_argument(
        "--skip-completeness-check", dest="skip_completeness_check", action="store_true",
        help="do not refuse a short/truncated arm (see capture_completeness) -- for "
             "inspecting a known-partial capture on purpose. Never use this to force a "
             "verdict out of a campaign that is still running.")

    p_split = sub.add_parser(
        "split",
        help="split a multi-run HTTP capture into one file per run (<outdir>/<name>_run1.jsonl, _run2.jsonl, ...)",
    )
    p_split.add_argument("path")
    p_split.add_argument("outdir")

    args = parser.parse_args(argv)

    if args.cmd == "run":
        try:
            rows = load_run(args.path, run_index=args.run)
        except (FileNotFoundError, OSError, la.MultiRunError, IndexError) as exc:
            print(f"error: {exc}")
            return 1
        if not rows:
            print(f"error: no parseable rows in {args.path}")
            return 1
        try:
            resolved_min_segment_index = resolve_min_segment_index(
                args.path, explicit=args.min_segment_index)
        except StabilizationMismatchError as exc:
            print(f"error: {exc}")
            return 1
        metrics = compute_run_metrics(rows, band_c=args.band, min_segment_index=resolved_min_segment_index)
        if args.json:
            print(json.dumps(_jsonable(metrics), indent=2))
        else:
            print(format_single_run_text(args.path, metrics))
    elif args.cmd == "compare":
        artifact = None
        if args.noise_floor_path.lower() != "none":
            artifact, load_reason = nf.load_artifact_diagnostic(args.noise_floor_path)
            if artifact is None and load_reason is not None:
                # Loud, not fatal: a failed load degrades every verdict below
                # from "measured" to "UNKNOWN" (see NOISE_FLOOR_NOTE) -- that
                # degradation must be visible here, not just inferable from
                # the absence of the word "measured" further down.
                print(
                    f"WARNING: noise-floor artifact NOT LOADED ({load_reason}). "
                    "Comparisons below fall back to NOISE FLOOR: UNKNOWN -- "
                    "every verdict is PROVISIONAL, not a confirmed result."
                )
        report = compare_runs(args.path_a, args.path_b, band_c=args.band,
                               run_index_a=args.run_a, run_index_b=args.run_b,
                               noise_floor_artifact=artifact,
                               sensitivity_paths=args.sensitivity_from,
                               min_segment_index=args.min_segment_index,
                               check_completeness=not args.skip_completeness_check)
        print(compare_report_to_json(report) if args.json else format_compare_text(report))
        return 1 if "error" in report else 0
    elif args.cmd == "split":
        try:
            paths = hc.write_split_runs(args.path, args.outdir)
        except (FileNotFoundError, OSError) as exc:
            print(f"error: {exc}")
            return 1
        if not paths:
            print(f"error: no parseable rows in {args.path}")
            return 1
        for p in paths:
            print(p)
    else:
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
