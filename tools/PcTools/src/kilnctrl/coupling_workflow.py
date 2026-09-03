"""coupling_workflow.py -- ONE command wiring run_queue.py (firing queue),
coupled_ident.py (settle audit / matrix assembly / scoring) and
config_presets.py (preset schema) into a repeatable "re-identify the kiln's
coupling matrix" pass. This module reimplements NONE of those three --
it only sequences calls into them exactly as tonight's manual procedure did
by hand (firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.2).

WHAT ONE RUN DOES, in order:

  1. Three single-zone excitation firings (profiles #4/#5/#6 -- zone0/1/2
     alone, 55C, 35-min dwell -- see DEFAULT_PROFILE_IDS), each from rested,
     via ``run_queue.run_entry`` -- one ``QueueEntry`` per zone, each with
     its own log file (``cpl_z{zone}.jsonl``), so two runs can never land in
     one file (the exact hazard ``run_queue._numbered_log_path`` exists for;
     this module gets the same guarantee for free by construction: one file
     name per zone, never reused across zones).
  2. A settle audit of every dwell window in every capture
     (``coupled_ident.settle_criterion_audit_from_paths``).
  3. THE REFUSAL GATE (:func:`settle_gate`): if the capture's OWN driven
     zone's dwell failed the firmware's settle criterion, or passed it but
     was flagged unstable by the tightened sec 3.2 duty-range check, this
     raises :class:`CouplingWorkflowError` naming exactly which zone and
     capture -- no matrix is ever assembled from data that failed this
     gate. This is the automation of the exact mistake the first coupling
     matrix made: assembling from an unsettled dwell because nothing
     stopped it.
  4. Matrix assembly in ``[affected][stepped]`` orientation, plus the
     transpose (``coupled_ident.matrix_from_single_zone_columns``).
  5. Scoring: condition number + plausibility
     (``coupled_ident.matrix_plausibility``), the sec 3.2 bias metric
     (``coupled_ident.score_matrix`` against dwell-observation fixtures) AND
     the transient metric (``coupled_ident.score_matrix_transient``, commit
     cbf38c9) over the freshly-captured ramp/dwell-entry data itself -- each
     reported under its own key, never conflated (see
     ``score_matrix_transient``'s own module comment for what each one
     validates and does not).
  6. A ready-to-apply preset file in ``config_presets.py``'s schema
     (:func:`build_preset`), overlaid on an existing base preset's
     non-coupling fields -- see that constant's own comment for why this
     never invents a PID gain, a relay mask, or a temperature limit; it only
     ever changes ``coupling_coeff``.

RESUMABILITY. ``run_queue.run_entry`` itself is not idempotent, and
``run_queue.py`` is owned by tonight's live campaign and must not be
edited to make it so. Resumability here is added ON TOP, at the
granularity of one whole capture (:func:`capture_is_complete`): before
firing zone Z, check whether ``cpl_z{Z}.jsonl`` already holds a run whose
last zone-bearing row reached a terminal ``profile_exec`` state (``done``
or ``faulted``) -- run_queue's own definition of "the run is over", not a
weaker heuristic. If so, that firing is SKIPPED entirely (no preset
re-apply, no rested wait) and the existing file is reused as-is. A capture
cut off mid-run (no terminal row) is NOT complete and is refired from
scratch: ``run_entry`` always starts with apply-preset + wait-rested, so
refiring from zero is always safe on its own terms, it just discards
whatever partial data the interrupted attempt captured -- ``run_entry``
opens ``log_path`` with mode ``"w"``, so a re-fired capture overwrites
rather than appends.

DRY RUN. ``--dry-run`` (or ``dry_run=True``) skips every HTTP/hardware
call -- ``run_queue.run_entry`` is never invoked -- and instead requires
the three capture paths to already exist
(``--zone0-capture``/``--zone1-capture``/``--zone2-capture``), driving
stages 2-6 exactly as a live run would, over already-captured data. This
is the mode this module's own test suite (and this task's own
instructions) always uses: it never touches a board.

CAPTURE FORMAT. ``run_queue.run_entry`` writes the ``{"t","exec","status"}``
envelope (one JSON object per line, no wall-clock prefix) -- the shape
``http_capture_log.py`` parses, NOT the ``HH:MM:SS {body}`` shape
``log_analysis.parse_profile_exec_jsonl`` (and hence ``coupled_ident``'s own
``*_from_paths`` helpers) expect. So this module reads every capture it
produces or is handed via ``http_capture_log.poll_rows`` into a
``log_analysis.PollRow`` sequence first, then calls ``coupled_ident``'s
ROW-based functions (``settle_criterion_audit``,
``single_zone_column_observations``, ``score_matrix_transient``) directly --
never its ``*_from_paths`` wrappers, which would silently mis-parse this
envelope (an ``exec``/``status``-wrapped line has no top-level ``zones``/
``dwelling`` key, so ``parse_profile_exec_jsonl`` would skip every line and
report zero observations rather than raise). The sec 3.2 fixtures scored in
stage 5 ARE in the plain format (that's what they are checked in as), so
``coupled_ident.dwell_observations_from_paths`` is used unchanged for those.

MULTI-RUN FILES. Several checked-in captures under ``logs/coupling/``
hold more than one run appended to the same file (see
``log_analysis.MultiRunError``'s own history for how that happens and why
silently picking one is a defect). Every stage here that reads a capture
either delegates to a ``coupled_ident.*_from_paths`` helper (which already
iterates ``log_analysis.split_runs`` internally, using every run, not just
the last) or, for :func:`_pooled_transient_scores`'s per-capture transient
score, explicitly pools over EVERY non-empty run in the file rather than
indexing ``[-1]`` -- see that function's docstring.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import logging
import math
import os
from typing import Optional, Sequence

import numpy as np

from kilnctrl import config_presets
from kilnctrl import coupled_ident as ci
from kilnctrl import http_capture_log as hc
from kilnctrl import log_analysis as la
from kilnctrl import run_queue as rq

log = logging.getLogger(__name__)

ZONES = ci.ZONES

#: Mirrors run_queue._TERMINAL_STATES -- profile_exec states that mean "the
#: run is over" (dashboard_http.c's exec_state_name()). Duplicated as a
#: local literal rather than reaching into run_queue's private name, so this
#: module's resumability check does not silently drift if that module's
#: internal spelling ever changes without this one being told.
_TERMINAL_STATES = frozenset({"done", "faulted"})

#: Profile IDs for the three single-zone excitation firings this workflow
#: automates: zone0/1/2 alone, 55C, 35-min dwell -- tonight's manual
#: procedure (PID_EXPANSION_PLAN.md sec 3.2/3.x).
DEFAULT_PROFILE_IDS = {0: 4, 1: 5, 2: 6}


class CouplingWorkflowError(RuntimeError):
    """A safety/data-quality refusal (unsettled dwell, incomplete matrix,
    missing dry-run capture) or a propagated I/O/HTTP failure from a wired-
    in stage. Never silently skipped or downgraded to a warning."""


# ---------------------------------------------------------------------------
# Stage 1: fire (or reuse) the three single-zone captures.
# ---------------------------------------------------------------------------

def capture_log_path(out_dir: str, zone: int) -> str:
    return os.path.join(out_dir, f"cpl_z{zone}.jsonl")


def capture_is_complete(log_path: str) -> bool:
    """True iff ``log_path`` already holds a run whose last zone-bearing row
    reached a terminal ``profile_exec`` state (``done``/``faulted``) -- see
    module docstring's RESUMABILITY section. A missing file, an unparsable
    file, a file with no zone-bearing rows at all, or a file whose last such
    run never reached a terminal state (interrupted mid-firing) is NOT
    complete.
    """
    if not os.path.isfile(log_path):
        return False
    try:
        rows = hc.poll_rows(log_path)
    except OSError:
        return False
    if not rows:
        return False
    runs = la.split_runs(rows)
    nonempty = [r for r in runs if any(rr.zones for rr in r)]
    if not nonempty:
        return False
    return nonempty[-1][-1].state.lower() in _TERMINAL_STATES


def _load_capture_runs(log_path: str) -> "list[list[la.PollRow]]":
    """Every non-empty run in an ``http_capture_log``-format capture (the
    shape ``run_queue.run_entry`` writes) -- see module docstring's CAPTURE
    FORMAT section for why this is ``http_capture_log.poll_rows`` + a manual
    ``split_runs``, not ``coupled_ident``'s ``*_from_paths`` helpers."""
    rows = hc.poll_rows(log_path)
    return [r for r in la.split_runs(rows) if any(rr.zones for rr in r)]


@dataclasses.dataclass
class CaptureResult:
    zone: int
    log_path: str
    #: True if this capture already existed complete and firing was skipped.
    skipped: bool


def ensure_single_zone_captures(host: str, preset_name: str, out_dir: str,
                                 profile_ids: Optional[dict] = None,
                                 rq_cfg_kwargs: Optional[dict] = None,
                                 control=None) -> "list[CaptureResult]":
    """Fire (or reuse -- see :func:`capture_is_complete`) the three
    single-zone excitation captures, one ``run_queue.QueueEntry`` per zone
    via ``run_queue.run_entry`` directly (never ``run_queue.run_queue``, so
    a completed capture can be skipped without going through the other two
    zones' entries at all).
    """
    profile_ids = profile_ids if profile_ids is not None else DEFAULT_PROFILE_IDS
    os.makedirs(out_dir, exist_ok=True)
    cfg = rq.RunQueueConfig(host=host, **(rq_cfg_kwargs or {}))
    results = []
    for zone in ZONES:
        log_path = capture_log_path(out_dir, zone)
        if capture_is_complete(log_path):
            log.info("zone %d: reusing already-complete capture %s", zone, log_path)
            results.append(CaptureResult(zone=zone, log_path=log_path, skipped=True))
            continue
        entry = rq.QueueEntry(preset_name=preset_name, profile_id=profile_ids[zone],
                               log_path=log_path, label=f"zone{zone}-excitation")
        rq.run_entry(entry, cfg, control=control)
        results.append(CaptureResult(zone=zone, log_path=log_path, skipped=False))
    return results


# ---------------------------------------------------------------------------
# Stage 2/3: settle audit and the refusal gate.
# ---------------------------------------------------------------------------

def active_zone_settle_failures(log_path: str, active_zone: int) -> "list[ci.SettleAuditEntry]":
    """The subset of ``settle_criterion_audit``'s entries for THIS capture's
    own driven zone that either never settled or settled but were flagged
    unstable -- the only entries :func:`settle_gate` acts on.

    Passive zones in a single-zone-excitation capture routinely report
    ``settled=False`` under this plain audit call: they sit near ambient at
    ~0 duty by design, and the plain audit's ``_zone_settle_row`` call
    requires the ordinary above-floor duty (``coupled_ident.
    single_zone_column_observations`` uses a separate, longer, duty-floor-
    exempt settle window specifically because passive zones need one) -- so
    a passive zone's own "unsettled" entry must never gate a refusal here.
    """
    entries = []
    for run_idx, run_rows in enumerate(_load_capture_runs(log_path)):
        label = log_path if run_idx == 0 else f"{log_path}#run{run_idx}"
        entries.extend(ci.settle_criterion_audit(run_rows, source=label))
    return [e for e in entries
            if e.zone == active_zone and (not e.settled or e.flagged_unstable)]


def settle_gate(captures: Sequence[CaptureResult]) -> None:
    """Raise :class:`CouplingWorkflowError`, naming every offending
    (zone, capture, dwell) triple, if ANY capture's own driven zone failed
    the tightened settle criterion. This is the check the first coupling
    matrix shipped without -- see module docstring point 3.
    """
    problems = []
    for cap in captures:
        for f in active_zone_settle_failures(cap.log_path, cap.zone):
            reason = ("never settled" if not f.settled
                      else "settled but FLAGGED unstable (duty still swinging after the "
                           "settle instant -- see coupled_ident.UNSTABLE_DUTY_RANGE_ABS/FRAC)")
            problems.append(
                f"zone{cap.zone} ({cap.log_path}): dwell target={f.dwell_target_c}C -- {reason}")
    if problems:
        raise CouplingWorkflowError(
            "refusing to assemble a matrix -- the following dwell(s) failed the tightened "
            "settle criterion (PID_EXPANSION_PLAN.md sec 3.2):\n  " + "\n  ".join(problems))


# ---------------------------------------------------------------------------
# Stage 4/5: assemble + score.
# ---------------------------------------------------------------------------

def _pooled_transient_scores(matrix: np.ndarray, list_of_row_lists: Sequence[list]
                              ) -> "list[ci.ZoneScore]":
    """``coupled_ident.score_matrix_transient``, pooled across every run in
    ``list_of_row_lists`` (one entry per non-empty run) rather than only the
    last one. A capture file with more than one run appended
    (``log_analysis.split_runs``) must never have its earlier run(s)
    silently dropped -- see module docstring's MULTI-RUN FILES section, and
    ``log_analysis.MultiRunError``'s own history for the near-miss this
    guards against.

    Pooling is done via each run's own ``(n, mean, rms)`` triple rather than
    concatenating raw per-row errors, which is mathematically identical for
    these two reductions: the pooled mean is the n-weighted mean of the
    per-run means, and the pooled rms is sqrt of the n-weighted mean of the
    per-run rms^2 (rms^2 IS the mean of squared errors, so this is an exact
    two-level reduction, not an approximation).
    """
    totals = {z: [0, 0.0, 0.0] for z in ZONES}  # n, n*mean, n*rms^2
    for rows in list_of_row_lists:
        for s in ci.score_matrix_transient(matrix, rows):
            if s.n == 0:
                continue
            totals[s.zone][0] += s.n
            totals[s.zone][1] += s.n * s.mean_error
            totals[s.zone][2] += s.n * (s.rms_error ** 2)
    out = []
    for z in ZONES:
        n, weighted_mean, weighted_sq = totals[z]
        if n == 0:
            out.append(ci.ZoneScore(zone=z, n=0, mean_error=float("nan"), rms_error=float("nan")))
        else:
            out.append(ci.ZoneScore(zone=z, n=n, mean_error=weighted_mean / n,
                                     rms_error=math.sqrt(weighted_sq / n)))
    return out


def assemble_and_score(captures: Sequence[CaptureResult],
                        sec32_paths: Optional[Sequence[str]] = None) -> dict:
    """Run the refusal gate, then assemble the matrix and every score this
    task requires. Raises :class:`CouplingWorkflowError` if the gate fails
    (see :func:`settle_gate`) or the assembled matrix is incomplete (see
    ``coupled_ident.matrix_from_single_zone_columns``'s own "any cell with
    zero observations" refusal).
    """
    settle_gate(captures)  # never assemble from data that failed the gate

    column_obs = {}
    for cap in captures:
        obs = []
        for run_idx, run_rows in enumerate(_load_capture_runs(cap.log_path)):
            label = cap.log_path if run_idx == 0 else f"{cap.log_path}#run{run_idx}"
            obs.extend(ci.single_zone_column_observations(run_rows, cap.zone, source=label))
        column_obs[cap.zone] = obs
    matrix, coverage = ci.matrix_from_single_zone_columns(column_obs)
    if matrix is None:
        raise CouplingWorkflowError(
            "assembled matrix is INCOMPLETE -- at least one of the 9 [affected][stepped] "
            f"cells has zero observations: coverage={ {f'{i},{j}': n for (i, j), n in coverage.items()} }")

    condition_number = float(np.linalg.cond(matrix))
    plausible, plausibility_reason = ci.matrix_plausibility(matrix)

    result: dict = dict(
        matrix=matrix.tolist(),
        matrix_transposed=matrix.T.tolist(),
        coverage={f"{i},{j}": n for (i, j), n in coverage.items()},
        coverage_note=ci.coverage_redundancy_note(coverage),
        condition_number=condition_number,
        plausible=plausible,
        plausibility_reason=plausibility_reason,
    )

    # sec 3.2 bias metric -- the settled-dwell u_pred-vs-u_actual figure.
    # Scored against dwell-observation fixtures (defaults to
    # coupled_ident's own checked-in sec 3.2 fixtures); an empty list means
    # "do not score this", not "refuse" -- a fresh board with no such
    # fixtures yet should still get a matrix and a preset.
    sec32_paths = list(sec32_paths) if sec32_paths is not None else list(ci.DEFAULT_SEC32_FIXTURES)
    sec32_obs = ci.dwell_observations_from_paths(sec32_paths) if sec32_paths else []
    result["sec32_paths"] = sec32_paths
    result["sec32_scores"] = (
        [dataclasses.asdict(s) for s in ci.score_matrix(matrix, sec32_obs)] if sec32_obs else None)

    # transient metric (commit cbf38c9) -- over the freshly-captured
    # ramp+dwell-entry data itself, one pooled score per capture.
    transient_scores = {}
    for cap in captures:
        nonempty = _load_capture_runs(cap.log_path)
        if not nonempty:
            continue
        transient_scores[f"zone{cap.zone}"] = [
            dataclasses.asdict(s) for s in _pooled_transient_scores(matrix, nonempty)]
    result["transient_scores"] = transient_scores

    return result


# ---------------------------------------------------------------------------
# Stage 6: emit a loadable preset.
# ---------------------------------------------------------------------------

def build_preset(matrix: np.ndarray, base_preset_name: str, out_name: str,
                  description: Optional[str] = None) -> dict:
    """Overlay ``matrix``'s ``[affected][stepped]`` rows onto
    ``base_preset_name``'s zone entries as each zone's own
    ``coupling_coeff`` row -- exactly ``config_presets/
    coupling_matrix_20260831.json``'s own documented convention (see that
    file's header comment): zone i's ``coupling_coeff`` list IS matrix row
    i, entry j is how many degrees zone i rises per unit duty when zone j
    is stepped. The diagonal entry is forced to 0.0 regardless of what the
    fit produced -- the firmware force-ranges it to ``[0,0]`` on POST
    (``zones_http_handlers.c``) regardless of what is sent, so writing
    anything else there would just be a number the board silently discards.

    Every OTHER per-zone field (relay_mask, control_mode, cal_offset_c, PID
    gains, max_ramp_c_per_hr, max_temp_c, min_temp_c) is copied VERBATIM
    from ``base_preset_name`` -- this never invents a PID gain or a
    temperature limit, it only ever changes the coupling matrix, same
    discipline ``coupling_matrix_20260831.json`` itself documents.
    """
    base = config_presets.load_preset_data(base_preset_name)
    zones = []
    for zone in base["zones"]:
        idx = zone["index"]
        new_zone = dict(zone)
        if idx in ZONES:
            row = [float(v) for v in matrix[idx]]
            row[idx] = 0.0
            new_zone["coupling_coeff"] = row
        zones.append(new_zone)
    preset = dict(base)
    preset["name"] = out_name
    preset["description"] = description or (
        f"Coupling matrix assembled by coupling_workflow.py from a fresh single-zone "
        f"identification pass, overlaid on {base_preset_name!r}'s other fields (relay/PID/"
        f"limits unchanged).")
    preset["zones"] = zones
    return preset


def write_preset(preset: dict, out_dir: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{preset['name']}.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(preset, fh, indent=2)
        fh.write("\n")
    return path


# ---------------------------------------------------------------------------
# Top-level orchestration.
# ---------------------------------------------------------------------------

def run_identification(*, host: Optional[str] = None, preset_name: str, out_dir: str,
                        preset_out_name: Optional[str] = None,
                        dry_run: bool = False,
                        zone_captures: Optional[dict] = None,
                        profile_ids: Optional[dict] = None,
                        sec32_paths: Optional[Sequence[str]] = None,
                        rq_cfg_kwargs: Optional[dict] = None,
                        control=None) -> dict:
    """The one entry point this module exists to provide.

    ``dry_run=True`` skips every HTTP/hardware call -- ``run_queue.run_entry``
    is never invoked -- and requires ``zone_captures`` (``{zone: log_path}``,
    one entry per zone, every path must already exist) instead of firing
    anything. ``dry_run=False`` fires (or resumes, see
    :func:`ensure_single_zone_captures`) the three captures against
    ``host``.

    Returns a dict: ``captures`` (per zone: log_path, skipped), everything
    :func:`assemble_and_score` returns, and ``preset_path``/``preset_name``
    for the emitted, ready-to-apply preset. Raises
    :class:`CouplingWorkflowError` -- never returns a partial result -- if
    the refusal gate fires, the matrix is incomplete, or (dry-run only) a
    named capture path does not exist.
    """
    if dry_run:
        zone_captures = zone_captures or {}
        missing_zones = sorted(set(ZONES) - set(zone_captures))
        if missing_zones:
            raise CouplingWorkflowError(
                f"--dry-run requires an existing capture path for every zone; missing "
                f"{missing_zones}")
        captures = []
        for zone in ZONES:
            path = zone_captures[zone]
            if not os.path.isfile(path):
                raise CouplingWorkflowError(f"--dry-run: zone{zone} capture not found: {path}")
            captures.append(CaptureResult(zone=zone, log_path=path, skipped=True))
    else:
        if not host:
            raise CouplingWorkflowError("host is required unless dry_run=True")
        captures = ensure_single_zone_captures(
            host, preset_name, out_dir, profile_ids=profile_ids,
            rq_cfg_kwargs=rq_cfg_kwargs, control=control)

    scored = assemble_and_score(captures, sec32_paths=sec32_paths)

    matrix = np.array(scored["matrix"])
    out_name = preset_out_name or f"{preset_name}_reidentified"
    preset = build_preset(matrix, preset_name, out_name)
    # Validate immediately, using config_presets' own schema check, so a
    # malformed emitted preset is caught here rather than the next time
    # something tries to load it.
    config_presets._validate(preset["name"], preset)  # noqa: SLF001 -- read-only reuse
    preset_path = write_preset(preset, out_dir)

    return dict(
        captures=[dataclasses.asdict(c) for c in captures],
        **scored,
        preset_path=preset_path,
        preset_name=out_name,
    )


def format_result_text(result: dict) -> str:
    lines = ["=== captures ==="]
    for c in result["captures"]:
        lines.append(f"  zone{c['zone']}: {c['log_path']} ({'reused' if c['skipped'] else 'fired'})")
    lines.append("")
    lines.append("=== assembled matrix [affected][stepped] ===")
    for row in result["matrix"]:
        lines.append("  " + " ".join(f"{v:9.4f}" for v in row))
    lines.append(f"coverage: {result['coverage']}")
    lines.append(f"coverage note: {result['coverage_note']}")
    lines.append(f"condition number: {result['condition_number']:.4g}")
    lines.append(f"plausible: {'PASS' if result['plausible'] else 'FAIL: ' + result['plausibility_reason']}")
    lines.append("")
    lines.append("=== sec 3.2 bias score ===")
    if result["sec32_scores"] is None:
        lines.append("  (no sec32_paths given/found -- not scored)")
    else:
        for s in result["sec32_scores"]:
            lines.append(f"  zone{s['zone']}: n={s['n']:3d} mean={s['mean_error']:+.4f} rms={s['rms_error']:.4f}")
    lines.append("")
    lines.append("=== transient score (commit cbf38c9), per capture ===")
    for name, scores in result["transient_scores"].items():
        lines.append(f"  -- {name} --")
        for s in scores:
            lines.append(f"    zone{s['zone']}: n={s['n']:4d} mean={s['mean_error']:+.4f} rms={s['rms_error']:.4f}")
    lines.append("")
    lines.append(f"preset written: {result['preset_path']}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kilnctrl-coupling-workflow",
        description="one-command re-identification of the kiln's 3x3 coupling matrix: fire (or "
                     "reuse) three single-zone excitations, audit dwell settle, assemble+score "
                     "the matrix, emit a ready-to-apply preset.")
    parser.add_argument("--host", default=None, help="board host/IP; required unless --dry-run")
    parser.add_argument("--preset", required=True, dest="preset_name",
                         help="config preset applied before each firing, and the base preset the "
                              "emitted matrix preset overlays (every non-coupling field is copied "
                              "from it verbatim)")
    parser.add_argument("--out-dir", required=True,
                         help="directory for the three captures and the emitted preset")
    parser.add_argument("--preset-out-name", default=None,
                         help="name for the emitted preset (default: '<preset>_reidentified')")
    parser.add_argument("--dry-run", action="store_true",
                         help="validate every stage against existing captures, no HTTP/hardware "
                              "calls at all -- requires --zone0-capture/--zone1-capture/"
                              "--zone2-capture")
    parser.add_argument("--zone0-capture", default=None)
    parser.add_argument("--zone1-capture", default=None)
    parser.add_argument("--zone2-capture", default=None)
    parser.add_argument("--profile-zone0", type=int, default=DEFAULT_PROFILE_IDS[0])
    parser.add_argument("--profile-zone1", type=int, default=DEFAULT_PROFILE_IDS[1])
    parser.add_argument("--profile-zone2", type=int, default=DEFAULT_PROFILE_IDS[2])
    parser.add_argument("--sec32-path", action="append", default=None, dest="sec32_paths",
                         help="dwell-observation fixture(s) to score the assembled matrix against "
                              "(repeatable); default: coupled_ident's own checked-in sec 3.2 "
                              "fixtures")
    parser.add_argument("--serial-port", default=None,
                         help="serial port for the UART CONTROL link, only if a queued preset "
                              "carries a thermal model -- see run_queue.py's own --serial-port "
                              "help. Ignored with --dry-run.")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    zone_captures = None
    if args.dry_run:
        zone_captures = {}
        for zone, path in ((0, args.zone0_capture), (1, args.zone1_capture), (2, args.zone2_capture)):
            if path:
                zone_captures[zone] = path

    control = None
    if args.serial_port and not args.dry_run:
        from kilnctrl.control import ControlClient
        from kilnctrl.serial_link import UartLink
        control = ControlClient(UartLink(args.serial_port))

    try:
        result = run_identification(
            host=args.host, preset_name=args.preset_name, out_dir=args.out_dir,
            preset_out_name=args.preset_out_name, dry_run=args.dry_run,
            zone_captures=zone_captures,
            profile_ids={0: args.profile_zone0, 1: args.profile_zone1, 2: args.profile_zone2},
            sec32_paths=args.sec32_paths, control=control)
    except CouplingWorkflowError as exc:
        log.error("coupling workflow refused: %s", exc)
        print(f"REFUSED: {exc}")
        return 1
    finally:
        if control is not None:
            control.close()

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(format_result_text(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
