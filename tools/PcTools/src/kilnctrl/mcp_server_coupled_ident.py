"""COUPLED_IDENT tools -- offline coupled-identification validator (see
coupled_ident.py).

Same shape as mcp_server_plant_sim.py: pure offline computation, no board
access, no serial link, no firmware source touched at runtime.
"""
from __future__ import annotations

from . import coupled_ident
from . import mcp_server as _srv


@_srv._tool()
def coupled_ident_report(paths: list[str], json_output: bool = False) -> str:
    """Fit + score + nonlinearity + self-check the coupling matrix against
    one or more ``/api/profile_exec`` poll captures (JSONL, ``HH:MM:SS
    {json}`` per line -- a multi-run file is split automatically; a
    still-growing capture from a live firing is fine, a truncated trailing
    line is simply skipped).

    Extracts every settled joint (all-3-zone) dwell observation using the
    firmware's OWN dwell-settle thresholds (180 s / 0.003 C/s /
    min duty 0.03 -- see coupled_ident.py's module docstring), then:

    1. Scores the CURRENT on-board matrix against those observations
       (mean/RMS of ``u_pred - u_actual`` per zone) and self-checks that
       against the known figures in
       ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.2
       (-0.086 / -0.007 / +0.108 on the checked-in plant_sim fixtures).
    2. Least-squares fits a NEW 3x3 matrix from the same observations,
       reporting the observation duty matrix's condition number and
       REFUSING the fit outright above ~1e4 (mirroring the firmware's own
       admitted conditioning bound) -- a confident fit from collinear data
       (every zone tracking the same setpoint, which every profile here
       does) is the specific failure mode this tool exists to catch, not a
       corner case to paper over.
    3. Scores the fitted matrix the same way, for a direct head-to-head
       against the current matrix on identical data.
    4. Reports per-zone prediction error as a function of dwell
       temperature (the nonlinearity signature: the current matrix's zone
       2 error grows with dwell temperature in the sec 3.2 data).

    A fit condition number well under the refusal threshold is NECESSARY
    but not sufficient for a trustworthy fit -- inspect the reported
    matrix's off-diagonal signs and magnitudes for physical plausibility
    (every entry should be positive: more duty on any zone should never
    predict LESS heat on another) before treating a passing fit as ready to
    apply; this tool does not gate on that itself.
    """
    report = coupled_ident.render_report(paths)
    if json_output:
        import json
        return json.dumps(report, indent=2)
    return coupled_ident.format_report_text(report)


@_srv._tool()
def coupled_ident_single_zone(
    zone0_paths: list[str] = [],
    zone1_paths: list[str] = [],
    zone2_paths: list[str] = [],
    score_against_paths: list[str] = [],
    json_output: bool = False,
) -> str:
    """Assemble a coupling matrix directly from up to three SINGLE-ZONE
    excitation captures -- one zone driven alone per capture, the other
    two passive -- rather than fitting it from ordinary same-setpoint
    firings. Pass captures for whichever zones you have; a zone with no
    paths contributes nothing and the assembled matrix comes back
    incomplete (refused, not filled with a fallback) until all three are
    supplied.

    THIS IS THE CLEAN EXPERIMENT for the determinability problem
    ``coupled_ident_report`` surfaces: an ordinary firing's duty vectors
    are (near-)collinear because every zone tracks the same setpoint, so
    no amount of ordinary dwell data pins down the off-diagonal terms.
    Driving one zone alone makes column j of the matrix a direct
    measurement (``dT_i / u_j`` at steady state) with nothing to be
    collinear with. The passive zones settle far slower than the driven
    one (cross-zone tau 620-730 s, dead time 135-158 s) -- this tool uses
    a 30+ minute settle window for them, NOT the short active-zone one, so
    each capture needs a correspondingly long dwell to be usable.

    Pass ``score_against_paths`` (ordinary poll captures) to also score
    the assembled matrix against real joint dwell observations, head-to-
    head comparable with ``coupled_ident_report``'s current/fit scores.
    """
    by_zone = {0: zone0_paths, 1: zone1_paths, 2: zone2_paths}
    column_obs = {
        z: coupled_ident.single_zone_column_observations_from_paths(paths, z)
        for z, paths in by_zone.items() if paths
    }
    matrix, coverage = coupled_ident.matrix_from_single_zone_columns(column_obs)
    scores = None
    if matrix is not None and score_against_paths:
        joint_obs = coupled_ident.dwell_observations_from_paths(score_against_paths)
        scores = coupled_ident.score_matrix(matrix, joint_obs) if joint_obs else None

    if json_output:
        import json, dataclasses
        out = dict(
            matrix=(matrix.tolist() if matrix is not None else None),
            coverage={f"{i},{j}": n for (i, j), n in coverage.items()},
            scores=([dataclasses.asdict(s) for s in scores] if scores is not None else None),
        )
        return json.dumps(out, indent=2)
    return coupled_ident.format_single_zone_report_text(matrix, coverage, scores)


@_srv._tool()
def coupled_ident_settle_audit(paths: list[str], json_output: bool = False) -> str:
    """Check whether the firmware's own dwell-settle criterion
    (180 s / 0.003 C/s on actual_c, no duty check at all -- see
    ``adaptive_tune.c`` / ``coupled_ident.py``'s ``_zone_settle_row``)
    would accept a reading that is still drifting or oscillating rather
    than genuinely at steady state.

    Built directly from a real finding on the coupid6 capture (10-minute
    dwells against a ~265 s tau): the settle test only watches actual_c's
    slope, so it can fire at a coincidental local flat spot in an
    under-damped oscillation while duty is still swinging widely for the
    rest of the same dwell -- a DC-gain reading taken there is not a
    DC-gain reading. This tool flags every "settled" reading whose duty
    still ranged more than 0.05 (absolute) or 25% of its own value
    (relative) over the REST of that same dwell window (after the settle
    instant, not the whole window -- the early ramp-in convergence is
    expected to swing and is excluded).

    Run this before trusting ANY dwell-derived observation -- from this
    module (``coupled_ident_report``) or by inference about what the
    firmware's own adaptive_tune.c harvest is doing on this board.
    """
    entries = coupled_ident.settle_criterion_audit_from_paths(paths)
    if json_output:
        import json, dataclasses
        return json.dumps([dataclasses.asdict(e) for e in entries], indent=2)
    return coupled_ident.format_settle_audit_text(entries)
