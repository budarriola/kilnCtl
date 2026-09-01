"""LOG_ANALYSIS tools -- windowed firing/tuning log analysis (see log_analysis.py).

Part of the mcp_server.py split, same shape as every other ``mcp_server_*``
group. Unlike its siblings this one touches no hardware, no serial link, and
no firmware source: it is a pure offline reader over poll-capture JSONL and
the board's own CSV exports, so it carries none of the connect/compatibility
plumbing the rest of this module needs.
"""
from __future__ import annotations

from . import log_analysis
from . import mcp_server as _srv


@_srv._tool()
def log_analyze(
    kind: str,
    path: str,
    trace_path: str = "",
    compare_path: str = "",
    band_c: float = 1.0,
    json_output: bool = False,
) -> str:
    """Analyze a captured firing or autotune log -- no board access, no serial link.

    Never reads raw poll rows for you to eyeball; it computes the numbers
    and hands back a report. Three modes, selected by ``kind``:

    ``kind="firing"`` -- windows a ``/api/profile_exec`` poll capture (one
    ``HH:MM:SS {json}`` line per poll) into per-segment ramp/dwell windows and
    reports, per zone per window: signed mean error, RMS error, max
    overshoot/undershoot with the time each occurred, a normalized IAE
    (time-weighted mean |error|, in C), and the duty range/mean. Also
    reports settle time and the duty-off -> temperature-peak lag after each
    ramp-to-dwell transition (``band_c`` sets the settle tolerance, default
    +/-1.0 C), saturation time (duty>=0.99 or <=0.01), any
    ff_hold_infeasible episodes, and sanity-check warnings (e.g. every zone
    running hot/cold on average -- a systematic over/under-drive).

    ``kind="autotune"`` -- summarizes an ``/api/autotune`` poll capture
    (fitted K/tau/dead-time, the three confidence flags, baseline vs
    step_ambient, raw_rise vs the extrapolation-corrected rise_inf, proposed
    PID, completed-or-aborted). Pass ``trace_path`` (an
    ``/api/autotune/trace.csv`` export) to also get an INDEPENDENT
    least-squares FOPDT refit of the raw trace and its percent difference
    from the firmware's own fit -- this is the cross-check that catches a
    tune whose fit disagrees with its own data.

    ``kind="compare"`` -- diffs two firings of the same profile (``path`` vs
    ``compare_path``): whole-run mean/RMS error and normalized IAE per zone,
    flagged improved/worse. Use this for a before/after check on a tracking
    fix.

    Set ``json_output=True`` for machine-readable JSON instead of the
    default compact text report.
    """
    kind = kind.strip().lower()
    if kind == "firing":
        report = log_analysis.render_firing_report(path, band_c=band_c)
        return (
            log_analysis.firing_report_to_json(report)
            if json_output else log_analysis.format_firing_report_text(report)
        )
    if kind == "autotune":
        report = log_analysis.render_autotune_report(path, trace_path or None)
        return (
            log_analysis.autotune_report_to_json(report)
            if json_output else log_analysis.format_autotune_report_text(report)
        )
    if kind == "compare":
        if not compare_path:
            return "error: kind='compare' requires compare_path (the second run to diff against)"
        report = log_analysis.compare_firing_runs(path, compare_path, band_c=band_c)
        return (
            log_analysis.compare_report_to_json(report)
            if json_output else log_analysis.format_compare_report_text(report)
        )
    return f"error: unknown kind {kind!r}, expected 'firing', 'autotune', or 'compare'"
