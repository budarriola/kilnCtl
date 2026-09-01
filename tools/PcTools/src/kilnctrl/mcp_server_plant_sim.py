"""PLANT_SIM tools -- calibrated offline plant/PID simulator (see plant_sim.py).

Same shape as mcp_server_log_analysis.py: pure offline computation, no
board access, no serial link, no firmware source touched at runtime.
"""
from __future__ import annotations

from . import plant_sim
from . import mcp_server as _srv


@_srv._tool()
def plant_sim_compare(
    path: str,
    run_idx: int = 0,
    kp: float = 0.06,
    ki: float = 0.0003,
    kd: float = 0.0,
    climb_mode: str = "coupled",
    integral_floor: str = "ff_hold",
    json_output: bool = False,
) -> str:
    """Simulate a hardware capture's OWN profile (ramp rate, segment timing
    and start temperature read off the capture itself -- never assumed) and
    diff the simulator's windowed per-zone stats against
    ``log_analysis``'s own windowed stats for that same capture.

    NOT A VALIDATED ORACLE -- read plant_sim.py's module docstring before
    trusting a result. In short: trustworthy for ramp-tracking magnitude
    and sign on ``climb_mode="coupled"`` builds (residuals against five
    calibration captures run 0.0-1.6 C, comparable to hardware's own
    0.7-2.8 C run-to-run RMS on the same windows). NOT quantitatively
    trustworthy for ``climb_mode="uncoupled"`` (gets the baseline defect's
    direction and rough scale right, not its saturation dynamics). Zone 2's
    second dwell on every coupled build runs 1.8-2.6 C cold vs hardware --
    this reproduces a known, still-open plant-identification gap
    (PID_EXPANSION_PLAN.md sec 3.2), not a simulator defect; do not read a
    zone-2 dwell number from this tool in isolation.

    ``climb_mode``: "coupled" (shipped Gaussian solve, 'after' onward) or
    "uncoupled" (baseline's per-zone defect formula).
    ``integral_floor``: "ff_hold" (shipped fix, holdfix/final) or "ff_u"
    (ifix's superseded whole-feedforward floor).
    """
    report = plant_sim.render_sim_vs_capture_report(
        path, run_idx=run_idx, kp=kp, ki=ki, kd=kd,
        climb_mode=climb_mode, integral_floor=integral_floor,
    )
    if json_output:
        return plant_sim_report_to_json(report)
    return plant_sim.format_sim_vs_capture_report_text(report)


def plant_sim_report_to_json(report: dict) -> str:
    import json
    return json.dumps(report, indent=2)
